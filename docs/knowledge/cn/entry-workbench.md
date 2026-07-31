# entry/workbench 架构知识

> **DIRTY（2026-07-31）：本文写的那个二进制已经不存在了，它描述的模型也已被替换。**
> 以实际代码、
> [`docs/plans/2026-07-31-annotation-model-capabilities.md`](../../plans/2026-07-31-annotation-model-capabilities.md)
> 和 [`entry-cli.md`](entry-cli.md) 为准，待重新同步。本条 banner 覆盖下面那条
> 2026-07-26 的；后者保留，作为此前已经积累的漂移记录。
>
> **2026-07-31 落了两件互相独立的改动。**
>
> *其一：GUI 被归档*（`b57b67b`，计划 §四之二.1）。从代码树里消失的有：
> `entry/workbench/app/`（`main.cpp`、`panels.*`）、Dear ImGui + Direct3D 11 外壳
> （`windows-gui-shell.*`、`windows-texture-cache.*`）、文件对话框、一次性 WGC 抓帧源
> （`windows-capture-source.*`）、imgui submodule，以及那个启动该可执行文件的 ASan
> smoke fixture。没有 `umbra-workbench` 这个二进制，也没有 `--smoke` 开关。下文每一处
> 面板、停靠、画布、快捷键、纹理和鼠标交互写的都是已归档的代码；它们留在 git 历史里。
>
> *`entry/workbench` 下留下来的是标注后端*，编译成静态库
> `${PROJECT_NAME}_workbench_support`，由 `umbra-authoring` 链接：编辑层
> （`authoring-edit.*`、`edit-page.*`、`authoring-actions.*`、`page-view.*`、
> `project-tree.*`、`panel-state.*`）、证伪矩阵（`preview.*`、`model-check-job.*`、
> `model-check-view.*`）、项目持久化（`project-persistence.*`，底下是
> `platform/windows-file-publication.*`）和源图导入（`source-ingestion.*`）。下文讲
> 编辑/校验/发布流程、以及「`AuthoringDocument` 是唯一写入路径」的那些段落在概念上仍然成立
> ——把「workbench 做 X」读成「标注后端做 X，由 CLI 驱动」即可。归档时记下两处纠缠：
> `EditPage` 的公开签名里仍然出现 `AppState` 与 `PanelUiState`，所以中间层在它被重新表达
> 之前搬不走；`project-persistence` 会调到 `windows-file-publication`，那是每一次
> `umbra-authoring` 保存背后的原子写，所以那个文件不算 GUI，也就没有跟着走。
>
> *其二：标注模型变了。* `AnnotationType`、`ElementKind`、`AnchorElement` /
> `InteractiveElement` / `InfoElement`、`bool shared`、`allowed_page_ids`、
> `retypeRecognizer`、公开的 `PageSignature::create` 以及 `derivedRuntimeRecognizerId`
> 全部消失；`RecognizerId` 改名 `ElementId`。逐条清单见
> [`module-annotation.md`](module-annotation.md) 的 banner。具体到本文：构造链不再经过
> `PageSignature::create`（签名由 `RecognitionCatalog::create` 从行使 `identify` 的页面
> 引用派生），而「selected recognizer 若为 `ActionTarget`」现在读作「选中的元素若声明了
> `interact`」——并且 `evaluateActionTarget` 还多收一个页面参数。

> **DIRTY（2026-07-26）**：本文尚未反映 page-centric 重构（EditPage/PageView
> 句柄层、authoring schema v2 的 Element+placement 模型、v1 路径退役、按
> placement 展开的运行时清单生成）。以实际代码与
> `docs/plans/2026-07-26-page-centric-authoring.md` 为准，待重新同步。

`umbra-workbench` 是 A1 阶段的 Windows 标注工具。GUI、采集和文件发布都围绕
`modules/annotation` 已有的编辑模型、编译器和识别接口组织，不另行定义 schema。
它是这些模块的组合入口；业务规则仍放在 `annotation`、`image`、`controller` 或
runtime 中。

## Workbench 负责什么

Workbench 拥有从“取得一张完整源图”到“发布可供 runtime 消费的生成闭包”的作者工作流：

- 从 PNG 或 WGC frame 构造带 provenance 的 canonical source；
- 保存 source selection、canvas view 和即时 Preview 等 session state；
- 把控件产生的修改统一变成完整、可验证的 `AuthoringDocument` 版本，并提供 undo/redo；
- 展示和编辑 recognizer、page、regression、`template_rect`、`search_roi`、threshold 和 click offset；
- 用当前内存 source 编译 template assets 和 runtime manifest；
- 用生产侧同一个 `RecognitionRuntime` 对选中 source 做有界 Preview；
- 按内容寻址资产优先、runtime manifest 最后的顺序发布项目。

以下规则不由 Workbench 定义：

- schema、引用闭合、矩形和 fingerprint 合法性属于 `modules/annotation/source/annotation/authoring-document.hpp`；
- template crop、runtime manifest 生成和 canonical ordering 属于 `modules/annotation/source/annotation/authoring-compiler.hpp`；
- SAD matching、page resolution、Unknown/Ambiguous 和 stop reason 属于 `modules/annotation/source/annotation/recognition-runtime.hpp` 及 `modules/vision/source/vision/sad.hpp`；
- PNG codec、pixel-layout conversion 和确定性 crop 属于 `modules/image/source/image/png.hpp` 与 `modules/image/source/image/pixels.hpp`；
- target discovery 与 WGC session 属于 `modules/controller/source/controller/discovery.hpp` 和 `modules/controller/source/controller/capture.hpp`；
- observation lease、action authorization、input delivery 和 trace 属于 runtime/engine/controller 链。Workbench 没有 click sink，也不能把 Preview evidence 变成输入能力。

`entry/workbench` 不是新的领域模块。panel 不应复制校验、匹配或序列化规则；
作者输入必须交给现有的规范化工厂和编译器，panel 只负责收集输入和显示错误。

构建结构在 `entry/CMakeLists.txt` 中给出。`umbraflow_workbench_support` 收纳 ImGui-free 的 authoring backend、Preview、canvas math 与 app state，供 `test-workbench` 直接链接；`umbra-workbench` executable 才加入 `panels.cpp`、Win32/D3D11 shell、file dialog、texture cache 和 vendored Dear ImGui。
`windows-file-publication.cpp` 仍在 support target 中，且 WGC ingestion 也按 `controller` target 条件加入，所以这个 support target 是“无 ImGui”而不是已经跨平台的正式 module。

GUI 可按三层导航：

1. Platform shell：`entry/workbench/platform/windows-gui-shell.hpp` 的 `GuiShell` 拥有 Win32 window、DXGI swap chain、D3D11 device 与 ImGui context；`TextureCache`、file dialog、WGC adapter 和 publication adapter 也留在 `entry/workbench/platform/`。
2. ImGui-free app/backend：`AuthoringEditHistory`、`AppState`、ingestion、persistence、Preview 和 canvas math 只处理 project values、`Result` 与 owned bytes，不调用 ImGui。
3. Panels：`entry/workbench/app/panels.hpp` 暴露 `drawWorkbench`、`PanelUiState` 与
   `WorkbenchServices`；`panels.cpp` 把四个即时模式面板的操作转交给 app 方法。
   系统文件选择、捕获和纹理上传只通过仅在本次调用期间有效的服务回调进入。

`entry/workbench/app/main.cpp` 负责组合这三层：解析可选的项目根目录和 `--smoke`，
加载或创建 `AppState`，创建 `GuiShell`，绑定三个 `WorkbenchServices` 回调，再由
shell 每帧同步调用 `drawWorkbench`。只有 `dispatch`/`main` 会把错误写到 `stderr`。

## 编辑与发布流程

### 编辑状态与修改入口

`entry/workbench/authoring-edit.hpp` 定义供界面编辑的中间数据：
`AuthoringDraft` 聚合 `EditableSource`、`EditableRecognizer`、`EditablePage` 和
`EditableRegression`。控件可以暂时写入普通字符串、整数和 vector，但这些值不能
直接持久化。

`makeAuthoringDraft` 把规范化的 `annotation::AuthoringDocument` 展开成上述中间数据；
`buildAuthoringDocument` 再依次调用 `AuthoringSource::create`、
`ResourceName::create`、`SimilarityThreshold::create`、`TemplateOffset::create`、
`RecognizerDefinition::create`、`PageSignature::create`，最后调用
`AuthoringDocument::create`。
这条重建路径解释了为何 GUI 不做“先改半个对象、以后再校验”：一次 edit 要么得到完整有效的 document，要么原版本不动。

`AuthoringEditHistory::apply` 先完成重建，再比较新旧 document 的规范序列化结果。
相同 draft 返回 `false`，不写入历史；发生变化时，把当前 document 移入 undo、清空
redo，并把 undo 限制为 `k_maximumAuthoringUndoEntries == 100`。
`undo`/`redo` 移动完整 document value，所以跨 recognizer/page 的引用始终作为同一版本恢复。

`entry/workbench/workbench-app.hpp` 的 `AppState` 是窗口背后的
ImGui-free session aggregate：

- `m_history` 是 document 的唯一真相；
- `m_sources` 是按 `SourceId` 查找的 immutable source-asset cache；
- selection、`CanvasView`、last Preview 和 dirty flag 是 transient UI state；
- `applyEdit` 是一般 document mutation 的唯一入口；
- `addIngestedSource` 同时路由 document edit 和 asset cache 更新；
- `compilerSourceAssets` 按当前 document 的 source 顺序复制 cache entry。

cache 故意不随 undo 回滚。撤销 import 后，PNG 成为 harmless orphan；redo 可以直接复用它而无需重新 decode。`compilerSourceAssets` 只输出当前 document 引用的 entry，因此 compiler/Preview 看不到 orphan；若 document source 在 cache 中缺失，它返回 `InternalInvariant`，而不是悄悄少编一张图。
成功 save 后 `markSaved` 才调用 `pruneSourceCacheToDocument` 清除不可达 entry。

任意 committed edit、undo/redo 或 source selection 变化都会清掉 stale
Preview。dirty flag 则保守：undo 回已保存内容仍可能保持 dirty，因为 history
没有可恢复的 revision cursor；代码中的 `TODO(cpp-debt)` 已明确升级方向。

### 生成 ResourceId

`entry/workbench/workbench-app.cpp` 的 `mintResourceId` 用 `std::random_device` 填满 16 bytes，再设置 UUID version 4 nibble 和 RFC 4122 variant bits，最后调用 `modules/annotation/source/annotation/resource.hpp` 的 `ResourceId::fromBytes`。
`fromBytes` 本身按契约不验证 version/variant，因此 authoring caller 负责设置 convention。

`SourceId`、`ElementId`、`PageId` 等是 `ResourceId` 上的 distinct strong types；panel 在新增 source、recognizer 或 page 时先 mint，再包成对应 ID。随机性只决定新资源 identity，不进入 runtime matching。
ID 一旦进入 document，canonical compiler 以它作为稳定 ordering/reference key。

### 导入源图

PNG 路径从 `WorkbenchServices::m_pickPngToImport` 到
`entry/workbench/source-ingestion.hpp` 的 `importSourcePng`：

1. `image::loadPng` 解码外部文件；
2. `assembleSource` 用 decoded geometry 和 `dpi` 参数（默认 96）建
   `ProjectFingerprint`。2026-07-30（`eacb05f`）起它是**参数而不是常量**：`dpi` 必须是
   *这张截图所来自的窗口*的密度，而不是文件的密度——文件没有密度。`AuthoringDocument` 要求
   每个 source 的指纹与项目相等，所以按错密度导入不会让结果变差，而是让文档被拒绝；在它还被
   钉死为 96 的时候，一个从 144 dpi 窗口标注的项目一张文件都吃不进去；
3. pixels 经 pinned `image::encodeRgbaPng` 重新编码为 canonical PNG；
4. canonical bytes 经 `annotation::sha256` 得到 `ContentHash`；
5. 返回同 ID 的 `AuthoringSourceSpec` 与 `AuthoringSourceAsset`，
   provenance 为 `ImportedSourceProvenance`。

重新编码很关键：来源 encoder 不同但 pixels 相同的 PNG 会归一到项目自己的
byte form，后续内容寻址和重复保存不受外部 metadata/compression 选择影响。

WGC 路径由 `entry/workbench/platform/windows-capture-source.hpp` 承接。`captureSourceFromTargetTitle` 拒绝空 substring，从 `enumerateCandidates` 中选择第一个 title 包含 substring、visible 且非 iconic 的 window，解析 client origin/size，创建 one-shot `WgcCaptureSession`，再由 `captureSourceFromSession` 取一个 `Frame`。

`ingestSourceFromFrame` 只接受 `PixelFormat::Bgra8`。它以 full-frame `PixelRect` 通过 `image::cropBgra8` 去掉 stride padding，转换成 packed RGBA，再走同一个 canonical encode/hash assembler。
provenance 记录 frame 的 `TargetGeneration` 和 wall-clock RFC 3339 capture time；monotonic timestamp 不被错误序列化成日历时间。

空项目由 `AppState::createEmpty` 暂以 1280×720、96 DPI fingerprint 建立。
第一张 source 通过 `addIngestedSource` 时替换该 placeholder；后续 source 则由
完整 `AuthoringDocument::create` 校验必须与项目 fingerprint 相容。

### 保存、重开与发布顺序

写入口是 `entry/workbench/project-persistence.hpp` 的
`saveAndGenerateAuthoringProject`。真实顺序是：

1. 在创建目录或写 final metadata 前调用 `compileAuthoringDocument`，验证
   document 与全部 source bytes，并生成 `CompiledAuthoringProject`；
2. 建立 project root、`assets/sources`、`assets/templates`、`generated`，以及 2026-07-30
   （`2429578`）起多建的一个空 `tasks/`。workbench 不写任何 task，但运行时在
   `<projectRoot>/tasks/<name>.luau` 解析 task，于是在这里标注并保存的项目，作者不先自己
   想明白「少了一个目录、少的是哪个」就跑不起来。它保持为空；它在，运行时那句「没有这个
   task」才是在说 task，而不是在说目录布局；
3. 发布 document 引用的 content-addressed source PNG；
4. 发布编译所得 content-addressed template PNG；
5. 单文件原子替换 `annotations.toml`；
6. 最后单文件原子替换
   `generated/annotations.runtime.toml`，把它作为 runtime closure 的 commit
   point。

`entry/workbench/platform/windows-file-publication.hpp` 的
`publishImmutableFile` 对已存在文件逐 byte 校验：相同即成功，不同则
`InvalidResource`，绝不覆盖同一 hash path。新文件先写、flush 同目录 temporary
file，再用 `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)` 安装。
`replaceFileAtomically` 也先写并 flush sibling temporary file，再用
`MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH` 做 same-volume name
switch。每一个 metadata 文件本身原子，但整个 artifact set 没有 transaction。

必须牢记已文档化的非原子窗口：若第 6 步失败，第 5 步的新
`annotations.toml` 已可见，而旧 runtime manifest 仍是有效 runtime closure；
新的 content-addressed assets 也可能已经留下。代码不 rollback。这个 ordering
优先保证 runtime 从不看见引用尚未发布 asset 的新 manifest，代价是 authoring
view 与 runtime view 可能暂时不同步。

读入口 `loadAuthoringProject` 把 `annotations.toml` 限制在 16 MiB，交给
`parseAuthoringDocument`，再按 document order 读取每个
`assets/sources/<hash>.png`。每个 PNG 受 `image::k_maximumPngFileBytes`
限制，必须重新 hash 等于 document record，且 decoded width/height 必须等于
source fingerprint。返回的 `LoadedAuthoringProject` 保留原 PNG bytes，不重新
编码，因此 load 后直接 save 可维持 byte-identical source assets。

### 有资源上限的预览

`entry/workbench/preview.hpp` 的 `runPreview` 不含私有 matcher：

1. 要求 selected `SourceId` 存在；
2. 对当前 document 和 source assets 调用 `compileAuthoringDocument`；
3. 把 compiler 的 template PNG 组装成 `EncodedRuntimeTemplate`；
4. 调用 `annotation::RecognitionRuntime::create`；
5. 把 selected PNG decode 成 project-fingerprint-sized BGRA `Frame`；
6. 调用 `RecognitionRuntime::evaluatePage`；
7. selected recognizer 若为 `ActionTarget`，再调用
   `evaluateActionTarget`。

panel 构造的 `RecognitionPolicy` 同时给出 comparison budget 和 deadline；
API 还支持 `std::stop_token`。`PreviewResult` 保留 completed
`PreviewAnchorRow`、Resolved/Unknown/Ambiguous、resolved page ID，以及
`PreviewStop` 中的 recognizer ID 和 `SadSearchStopReason`。stop 不会被压成
`hit=false`。Preview frame 的 synthetic frame/session/generation identity 只为
满足真实 recognition API；结果不进入 document、history 或 action delivery。

### 取色键

2026-07-30（`c392161`）起，元素可以带一个 `ColourKey`，而取键这个动作发生在 workbench。
`entry/workbench/preview.hpp` 里的两个函数就是这个动作的全部模型：

- `sampleSourcePixel(asset, point)` 是吸管——取一个键就是取一个像素。
- `previewColourKeyMask(asset, templateRect, colourKey)` 返回从所在屏幕上裁下来的模板矩形，
  **该键蕴含的 mask 已经写进 alpha 通道——就是编译器会烘进去的那些字节**——外加
  `fullyKeptPixels` 与 `partiallyKeptPixels`。不带键时 mask 全不透明，这正是一个不带键的元素
  编译出来的样子。

**看见选中的像素才是要点**，这也是面板把 mask 叠回裁剪图上、而不是只报一个数字的原因。
一个键是关于「哪些是字形、哪些是背景」的一次猜测，而它的两种失败——几乎什么都没选中，
或者把画面也选进来了——在数字里都看不出来，在叠加图上都一眼可见。两个计数分开报，是因为它们
回答不同的问题：多少一定是文字，以及它周围的边有多软；部分保留的像素就是容差斜坡按较低权重
重新收回的抗锯齿边缘。

文档存的是键与容差，永远不是 mask，这样作者重开项目后还能拖动容差、看着选中的像素跟着变。
编译器拿它做什么见 `module-annotation.md`，匹配器怎么用它加权见 `module-vision-image.md`。

## 必须保持的约束

### Fail-closed

所有外部输入、decode、compile、capture、publish 和 recognition failure 都走
`Result<T>`/`Status`。draft validation 在 history mutation 前完成；project
compile 在目录创建和 metadata publish 前完成；load 对 missing、oversized、
hash mismatch 和 geometry mismatch 全部拒绝。

Recognition control stop 用 `PageRecognitionStop` 与 completed
`PageOutcome` 的 variant 表达。Unknown/Ambiguous 是完整评估后的显式 outcome，
但不能产生 `ResolvedPage`；Cancelled、TimedOut 和
ComparisonBudgetExhausted 连“不命中”都不冒充。Workbench 又不暴露 input
delivery，所以 Preview 无论何种结果都没有动作权限。

content-addressed destination 若已有不同 bytes，publication fail closed。这个
collision guard 保护“path 代表 bytes”不变量，也避免一次坏 save 覆盖 runtime
仍可能引用的旧资产。

### 确定性

确定性从 ingestion 延伸到 publication：source PNG 被 canonical re-encode；
template crop/PNG/hash 和 runtime TOML 由 `compileAuthoringDocument` 纯生成；
document equality 使用 canonical serialization；stable IDs 决定 canonical
ordering；相同 document 与 source bytes 产生相同 template bytes、hash 和
manifest。

authoring-time `mintResourceId` 是有意的非确定性边界。它只创建新 identity；
不改变已给定输入的 compile 或 recognition 结果。hit/miss 使用整数 SAD 与
integer basis-point threshold，Preview 直接调用 production
`RecognitionRuntime`，没有 GUI-only floating-point decision。

### 所有权与生命周期

document、draft、history entries、source bytes、compiled artifacts 和 Preview
结果均按 value/owned container 流动。`Frame` 的 pixel buffer 使用
`std::shared_ptr<FrameBuffer const>`，共享的是 immutable data。返回 reference
或 span 的 `AppState`/`RecognitionRuntime` accessor 标注
`UF_LIFETIME_BOUND`，调用者不能把 view 当 owner。

`GuiShell` 与 `TextureCache` 都以 move-only `std::unique_ptr` PIMPL 隔离 native
state。`GuiShellState` 按析构顺序释放 ImGui backends/context、D3D resources、
window 和 registered class。texture cache 持有 D3D device 与 shader-resource
view 的 owning COM references；暴露给 panel 的 `GpuSourceTexture` 只是 cache
lifetime 内有效的 opaque handle。根据
[2026-07-28 review follow-up](../../plans/2026-07-28-full-project-review-fixes.md)，
document edit 或 import 后，`drawWorkbench` 会通过
`WorkbenchServices::pruneTextures` 传递当前 source 列表；Windows 组合根把它转给
`TextureCache::pruneTo`，因此已删除 source ID 对应的 GPU view 会释放，不会在 shell
生命周期内持续累积。

`GuiFrameCallback` 由 `GuiShell::run` 同步调用且不保存；
`WorkbenchServices` callback 只在一次 draw 中借用。`main.cpp` 的 reference
captures 安全依赖这一同步协议：`state`、`services`、`ui` 和 `shell` 都覆盖
整个 `run` 调用。

### 平台与 SAFETY 边界

Win32 handle bit restoration、`GWLP_USERDATA` pointer conversion、COM out
parameters、D3D texture handle conversion、Win32 path pointer 和 file I/O
buffer 都限制在 `entry/workbench/platform/*.cpp`。每处 raw/native operation
旁有 `// SAFETY:`，header 尽量只暴露 value、RAII object、`Result` 和 opaque
integer handle；ImGui/D3D11/Win32 类型不进入 `AppState` 或 authoring backend。

strict-background input 在本 subsystem 不适用：Workbench 是可交互的普通 GUI，
但对目标只做 WGC capture，不发送 mouse/keyboard input。产品的后台投递纪律由
runtime controller action sink 执行；把 input API 加进 `WorkbenchServices`
会跨越现有职责边界，也违背
`docs/plans/2026-07-22-annotation-design.md` 明确排除的 “runtime input from the
workbench”。

## 依赖关系

入站边是用户/OS 到 Workbench：

- command line 提供 project root 或 `--smoke` frame budget；
- ImGui widgets 产生 draft edit、selection、canvas gesture 和 action request；
- file dialog 提供 PNG path；
- controller discovery/WGC 提供完整 BGRA `Frame`；
- disk 上的 `annotations.toml` 与 source PNG 提供 reopen 输入。

向内的类型转换集中在边界：PNG path/`Frame` 变成 `IngestedSource`，widget
buffer 变成 `AuthoringDraft`，disk bytes 变成 validated
`AuthoringDocument`/`AuthoringSourceAsset`。进入 app/backend 后不再携带 HWND、
D3D pointer 或 ImGui widget state。

出站边是 Workbench 到 reusable modules 与 filesystem：

- 对 `annotation` 传 document、source asset、recognition policy，取 validated
  document、compiled project 和 evidence；
- 对 `image` 传 encoded/decoded pixel bytes，取 canonical PNG 或 layout
  conversion；
- 对 `controller` 传 selected target handle/geometry，取 captured frame；
- 对 filesystem 发布 source PNG、template PNG、authoring TOML 和 runtime
  TOML；
- 对 panel 返回 status string、texture handle 和 transient Preview rows。

`umbra-workbench` 不链接 `engine`。与 runtime 共享的 recognition core 是
`annotation::RecognitionRuntime`；runtime 只消费 Workbench 最后发布的
`generated/annotations.runtime.toml` 和 template assets，两者之间没有进程内
接口。这条 build edge 曾经存在而无任何源码引用，已在 2026-07-26 移除——不要
为了 engine session、lease 或 action port 把它加回来，那会跨越现有职责边界。

跨边界的稳定标识是 strong `ResourceId` wrapper，跨磁盘的完整性凭据是
`ContentHash`，跨 capture/authoring 的兼容性凭据是
`ProjectFingerprint`，跨 recognition/UI 的信息是扁平
`PreviewAnchorRow`/`PreviewStop`。这些窄 value 防止 panels 依赖
`RecognitionRuntime` 私有 evidence layout 或 native platform objects。

## 测试

Windows 下 `tests/CMakeLists.txt` 把七个 synthetic 文件合成
`test-workbench`，直接链接 `umbraflow_workbench_support`，从而绕过 ImGui 和
真实 desktop：

- `tests/workbench/test-authoring-edit.cpp` 锁定完整 draft round trip、validated
  apply、invalid draft 不改 history、identical edit、redo branch/replay 和
  100-entry undo boundary。
- `tests/workbench/test-workbench-app.cpp` 锁定 UUID v4/variant、empty state、
  dirty/history、selection/view、orphan cache 过滤、redo 恢复 source、branch 后
  只编译新 source，以及 edit 后 Preview invalidation。
- `tests/workbench/test-canvas-math.cpp` 锁定 source/screen inverse、anchor-preserving
  zoom、zoom-scaled pan、grip priority/hit test，以及 resize/move 的 source bounds
  与 one-pixel minimum。
- `tests/workbench/test-source-ingestion.cpp` 锁定 PNG canonical encode/hash、
  non-PNG rejection、BGRA→RGBA WGC provenance、stride padding 去除和非 BGRA
  rejection。
- `tests/workbench/test-project-persistence.cpp` 锁定完整 save/reopen、单文件原子
  replacement、publish 前 validation、source asset 重排映射、immutable collision
  rejection、无 temporary residue、load round trip、hash mismatch 与 missing
  source rejection。它没有把整个 artifact set 误断言成 transaction。
- `tests/workbench/test-preview.cpp` 锁定 resolved page/anchor evidence、selected
  action evidence、zero-budget stop reason 和 absent source rejection，证明 UI
  wrapper 保留 runtime outcome。
- `tests/workbench/test-model-check-job.cpp` 锁定承载整个 model check 的后台
  job：恰好交付一次、错误浮出而非被吞掉、work 返回前一直 running、第二次
  start 不打断在飞的运行、被 discard 的运行永不到达 status line、被 discard
  的运行不阻塞下一次、析构会取消并 join worker。它用假 work 驱动
  `startWith`，因此全部不需要真实像素扫描。

`tests/workbench/test-real-regression.cpp` 是单独的 local-only `REAL` suite。
只有 `tests/assets/real-regression` 存在时 CMake 才注册它；它遍历未提交的真实
project，调用 `loadAuthoringProject` 和 annotation regression runner，对 recorded
expectation 做回放。CI 只依赖 synthetic fixtures，真实截图不进入 repository。

GUI shell、file dialog 和 title-based WGC capture 没有 deterministic unit test；
`--smoke` 只覆盖有限帧的 shell 启动/泵循环，真实 target capture 与视觉交互仍需
Windows 真机验证。修改 platform lifetime 或 D3D/ImGui wiring 时，不能用
`test-workbench` 绿灯替代 smoke/人工证据。

## 后续扩展

`docs/plans/2026-07-21-product-form-and-roadmap.md` 是产品节奏权威：
A2 扩展多 anchor/page 的 required/forbidden、Unknown/Ambiguous 和样本
Preview/Test；A3 扩展 batch、sample management 和 static regression UX。
这些功能应分别接到现有 `AuthoringDraft`/`applyEdit`、`runPreview`/
`PreviewResult` 和 annotation regression runner，不应在 panels 内建立旁路
document 或 matcher。

`docs/plans/2026-07-22-annotation-design.md` 是 S0 schema、recognition、artifact
和 workbench contract 权威。新增 annotation type、schema field、resolution
adaptation、OCR 或 recognition policy 语义时，先在该 contract 的后继权威和
`modules/annotation`/`vision` 中建立 canonical behavior，再把 editing control
接到 Workbench。

如果以后需要抽取 `modules/authoring`，现有
`umbraflow_workbench_support` 中不依赖 ImGui 的公开接口就是候选边界：
`AuthoringEditHistory`、ingestion、compile-facing persistence model、
Preview adapter、`AppState` 的 document/cache 规则及其 synthetic tests。
现有代码已经用 value/`Result` API 隔开 panels，因而 consumer 不需要认识
Dear ImGui。

但不能把整个 support target 原样移动：`project-persistence.cpp` 直接调用
Windows file-publication adapter，WGC ingestion 直接依赖 `controller`，而
`AppState` 同时含项目语义与 GUI 选择、视图状态。抽取时应
保留纯 authoring policy，令 publication/capture 继续由 entry 注入 port，
GUI-only state 留在 entry；否则新 module 只是换目录的 Windows composition
root。当前 `docs/plans/` 没有为 `modules/authoring` 指定独立 phase，因此这一段
这里只记录现存且可验证的扩展点，不代表迁移时序已经确定。

发布事务的加固也是独立扩展点。
`docs/plans/2026-07-23-engine-architecture.md` 明确把 Workbench publication
rollback-window fix 排除在当前 phase 外。未来可在
`saveAndGenerateAuthoringProject` 与 `windows-file-publication` 之间加入
generation directory/journal 或等价 commit protocol；在那之前必须维持
“assets first、authoring second、runtime manifest last”的现有 ordering，并在
错误处理与测试中承认非原子窗口。

Platform 扩展应继续通过 `WorkbenchServices` 和 `platform/` RAII wrapper：
新的 picker、capture selector 或 renderer 只替换 service/opaque texture
实现。Recognition 扩展则通过 `compileAuthoringDocument`、
`RecognitionRuntime` 和 `PreviewResult` 进入。用这两类扩展点区分系统能力
与 product semantics，是保持 Workbench 可导航、可测试且未来可抽取的关键。
