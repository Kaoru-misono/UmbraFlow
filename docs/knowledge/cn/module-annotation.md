# annotation 模块架构知识

本文说明 `modules/annotation` 当前实现的 S0 契约。设计权威是
`docs/plans/2026-07-22-annotation-design.md`；本文以现有代码为准，帮助新开发者定位实现、理解约束并找到安全的扩展位置。

## 职责与边界

`annotation` 是“作者标注数据”到“可授权运行时识别证据”之间的平台无关领域层。它拥有六类职责：

1. 定义稳定资源身份、项目指纹、识别器和页面签名，并在对象构造时闭合引用关系。
2. 定义 GUI authoring document 与 runtime manifest 的精确 S0 schema，以及唯一的 canonical TOML 字节形式。
3. 从完整源截图确定性裁剪、编码、哈希模板，并将 authoring document 编译为内容寻址的 runtime closure。
4. 用同一套有界灰度 SAD 识别路径评估页面锚点和动作目标，完整保留命中证据、工作量和停止原因。
5. 将页面候选集收敛为 `ResolvedPage`、`UnknownPage` 或 `AmbiguousPages`，不做优先级或启发式消歧。
6. 在坐标动作交付前验证页面能力、动作检测、观察租约和实时兼容性，给 engine 一个 fail-closed 的授权结果。

模块公开依赖 `core`、`domain`、`vision`，私有依赖 `image`，见
`modules/annotation/manifest.txt`。这个依赖方向体现了边界：

- `domain` 提供 `Frame`、`Detection`、`ObservationLease`、坐标空间和身份值；`annotation` 组合它们，但不重新定义捕获或租约语义。
- `vision` 拥有 `GrayImage`、`bgra8ToGray8` 和有界 `matchTemplateSad`；`annotation` 决定如何排序工作、设阈值和解释结果，不另造 matcher。
- `image` 只在实现内部负责 PNG 解码、像素布局转换、裁剪和编码，因此 consumer 不需要直接依赖图像 codec。

它刻意不拥有以下能力：

- 不捕获窗口、不发现目标、不投递点击，也不实现 Windows `PostMessage`。这些分别属于 controller adapter 和 engine composition。
- 不负责文件系统原子发布。workbench 的发布顺序在
  `entry/workbench/project-persistence.cpp`，engine 的磁盘装载边界在
  `modules/engine/source/engine/runtime-loader.cpp`。
- 不负责 Luau VM、脚本 AST 或 opaque handle 暴露；这里只验证 `ResourceName` 是可直接访问的 ASCII Luau member key，并提供 host 可包装的强类型资源。
- 不做浮点阈值决策、颜色/HSV/OCR/composite recognizer、分辨率缩放、页面优先级或 best-effort schema 兼容。
- 不保证“点击一定成功”。它只证明 annotation 侧授权条件成立；engine 仍须在 sink 调用前复验目标实例，controller 仍须在实际投递层验证 lease。

因此 `strict-background` 在这里体现为“不能授权可疑坐标动作”，而不是一个输入后端。真正的后台投递在模块之外；`annotation` 的责任是让 `Unknown`、`Ambiguous`、recognition stop、过期证据或不兼容几何都无法越过授权边界。

## 关键类型与数据流

### 身份、几何和 Catalog

公共领域表面从 `modules/annotation/source/annotation/catalog.hpp` 开始：

- `ResourceId` 保存 16 字节 UUID，`parse` 接受固定的 36 字符 UUID 形状，`toString` 输出小写 canonical 形式。
- `RecognizerId`、`PageId`、`SourceId` 和 `RegressionId` 是基于 `ResourceId` 的不同 `StrongValue`，避免跨资源类别误传。
- `ProjectId` 要求非空有效 UTF-8；`ResourceName` 要求非空 ASCII identifier 且不是 Luau 保留字。
- `ProjectFingerprint` 是非零的 `width`、`height`、`dpiX`、`dpiY`。S0 把 AnnotationSpace 固定为此 base resolution 下的整数 `FrameSpace`。
- `AnnotationType` 当前有 `PageAnchor`、`ActionTarget`、`InfoRegion`。S0 schema 的 recognizer kind 仍只有 `gray_template`。

`RecognizerDefinition::create` 是单个识别器的验证入口。输入 `RecognizerSpec` 后，它验证
`templateRect` 和 `searchRoi` 都位于项目范围内、模板尺寸能放进 ROI、阈值可计算、click 只属于
`ActionTarget` 且落在模板内、`PageAnchor` 不携带 page membership、`ActionTarget` 至少授权一个页面。随后它按 `PageId` 排序 `allowedPageIds` 并拒绝重复项。

`PageSignature::create` 接收 `PageSpec`，要求 `required` 与 `forbidden` 至少一边非空，分别按
`RecognizerId` 排序，拒绝重复和交集。这里的“非空”很重要：空签名不会成为隐式 fallback page。

`RecognitionCatalog::create` 再做跨资源闭包验证：

- recognizer 和 page 都按 UUID 排序；
- ID 与 name 在各自类别内唯一，并且 page 与 recognizer 之间也全局唯一；
- page signature 只能引用存在的 `PageAnchor`；
- 每个 `allowedPageIds` 必须指向存在的 page；
- 两个 page 不得拥有完全相同的 required/forbidden 集合。

构造成功后，`RecognitionCatalog` 还保存去重、按 UUID 排序的 `pageAnchorOrder`。页面识别和
`PageResolver` 都以它为唯一锚点评估顺序，所以输入容器顺序不会渗入运行结果。

`recognizers()`、`pages()`、`pageAnchorOrder()` 返回只读 `std::span`；
`findRecognizer()` 和 `findPage()` 返回 non-owning pointer。这些 view/pointer 都以 catalog 的生命周期为上界，声明处用 `UF_LIFETIME_BOUND` 或注释明确了约束。

### Canonical authoring document

`modules/annotation/source/annotation/authoring-document.hpp` 定义 schema
`umbraflow-authoring/v1`。主要类型是：

- `AuthoringSource`：稳定 `SourceId`、`ContentHash`、派生的
  `assets/sources/<hash>.png` 路径、`ProjectFingerprint` 与 `SourceProvenance`。
- `WgcSourceProvenance`：`TargetGeneration` 和 canonical RFC 3339 `capturedAt`；
  `ImportedSourceProvenance` 没有伪造的 WGC 字段。
- `AuthoringRecognizerSpec`：一个已验证的 `RecognizerDefinition` 加其 `SourceId`。
- `RegressionCase`：独立保存 `RegressionClassification` 与
  `RegressionExpectation`；positive/negative/confusable 不会暗改 resolved/unknown/ambiguous 期望。
- `AuthoringDocument`：拥有 `RecognitionCatalog`、sources、recognizer-source 关系和 regressions。

`AuthoringDocument::create` 是 document 级 validation gate。它把 source、recognizer 和 regression
按 UUID 排序，要求所有 source fingerprint 等于项目 fingerprint，闭合 annotation→source、
regression→source 和 resolved regression→page 引用，并保证 source、recognizer、page、regression
的 ID 全局唯一。每类表最多 4096 条，canonical 序列化结果最多 16 MiB。

序列化和解析在 `modules/annotation/source/annotation/authoring-document.cpp`。
底层 `detail::CanonicalTomlReader` 与 append helpers 位于
`modules/annotation/source/annotation/detail/canonical-toml.hpp` 和
`modules/annotation/source/annotation/detail/canonical-toml.cpp`。它不是通用 TOML parser，而是 S0
生成格式的窄语法：

- 每一行必须以 LF 终止，CR 被拒绝，文件自然要求一个末尾 newline；
- 字段和 table 必须按 writer 的固定顺序出现，未知字段、注释和额外尾部内容没有入口；
- unsigned integer 只接受十进制 canonical 拼写，数组分隔固定为 `", "`；
- string 只接受一行 basic-string 语法、有效 UTF-8 和 writer 支持的 canonical escapes；
- parser 构造对象后重新调用 `serializeAuthoringDocument`，只有字节完全相等才接受。

这个“parse → validate → serialize → byte compare”闭环是 round-trip discipline 的关键。它拒绝的不是 TOML 语义本身，而是任何偏离 GUI 唯一输出的表示方式；因此 generated/source documents 可以稳定哈希、稳定 diff，也不会悄悄吸收 schema drift。

### 确定性编译与 runtime manifest

`compileAuthoringDocument` 位于
`modules/annotation/source/annotation/authoring-compiler.hpp` 和
`modules/annotation/source/annotation/authoring-compiler.cpp`。输入是已验证的
`AuthoringDocument` 与一一对应的 `AuthoringSourceAsset` PNG bytes，输出
`CompiledAuthoringProject`：

- `RuntimeManifest`；
- canonical `runtimeManifestToml`；
- 去重并按路径排序的 `TemplateAsset` 集合。

编译顺序是确定性的：

1. source asset 按 `SourceId` 对齐 document，要求数量和 ID closure 完全一致。
2. 预先按 source ID 与 `templateRect` 建立唯一裁剪任务；相同 source/rect 只做一次。
3. 每个 source 先验 SHA-256，再解码并核对宽高；source 一次只解码一个。
4. `generateTemplateAsset` 通过
   `modules/annotation/source/annotation/template-asset.cpp` 调用
   `image::cropBgra8`、`image::bgra8ToRgba8`、`image::encodeRgbaPng`，最后对编码后的 PNG bytes 调用 `sha256`。
5. 生成路径固定为 `assets/templates/<hash>.png`；相同 hash 的相同 bytes 只保留一个资产，hash 相同但 bytes 不同则拒绝。
6. recognizer 与生成 hash 重新关联，创建 `RuntimeManifest` 并 canonical 序列化。

编译器在分配/处理前使用 checked arithmetic。当前实现限制总 source pixels 加唯一 template-task pixels
不超过 256 Mi-pixels，唯一生成模板 PNG bytes 总和不超过 512 MiB；单个 PNG 还受 image codec
的文件配额约束。配额或闭包失败返回 `InvalidResource`，函数只返回完整值，不发布任何部分结果。

`ContentHash` 和内置 SHA-256 实现在
`modules/annotation/source/annotation/content-hash.hpp` 与
`modules/annotation/source/annotation/content-hash.cpp`。文本形式严格为
`sha256:` 加 64 个小写十六进制字符。

`RuntimeManifest` 位于
`modules/annotation/source/annotation/runtime-manifest.hpp`。它拥有一个
`RecognitionCatalog` 和按 recognizer 对齐的 `RuntimeRecognizerAsset`：
`templateHash`、`sourceHash`、`templatePath`。runtime schema 是
`umbraflow-annotations/v1`，不包含 source ID、capture time、target generation 或完整截图。

`RuntimeManifest::create` 验证每个 recognizer 只有一个 asset，并把 template path 从 hash 派生为
`assets/templates/<hash>.png`。`parseRuntimeManifest` 使用同一 canonical reader，要求 recognizer
先于 page、每类最多 4096 条、文档最多 16 MiB，并在末尾通过
`serializeRuntimeManifest(manifest) == input` 拒绝非 canonical 输入。

### 阈值、运行时识别与 stop preservation

`SimilarityThreshold` 在 `modules/annotation/source/annotation/catalog.hpp` 中以 basis points
保存 `[0, 10000]`。`maximumSad(width, height)` 用 checked `uint64` 算术实现：

```text
maxSad = floor((10000 - basisPoints) * 255 * width * height / 10000)
hit    = sadScore <= maxSad
```

等号命中是契约的一部分。`AnchorEvaluation::fromSadOutcome` 位于
`modules/annotation/source/annotation/recognition.cpp`，它检查 matcher 返回矩形仍在 `searchRoi`
中、score 不超过理论最大 SAD，然后构造 `AnchorEvidence`。`displayConfidence` 是浮点展示值，
`hit` 只由整数 `sadScore <= maximumSad` 决定。

`RecognitionRuntime` 的公共表面在
`modules/annotation/source/annotation/recognition-runtime.hpp`：

- `create` 接受 `RuntimeManifest` 与 `EncodedRuntimeTemplate`。它要求收到的 hash 集合恰好等于 manifest
  引用的唯一模板集合，重新计算每份 PNG 的 SHA-256，解码为自有 Gray8 bytes，并核对模板尺寸与
  recognizer geometry。
- `evaluatePage` 返回 `PageRecognitionAttempt`，既可携带完整 `PageOutcome`，也可携带
  `PageRecognitionStop`，同时保留已经完成的 anchor evidence 和 pixel comparison 计数。
- `recognizePage` 是操作型便利入口：completed outcome 原样返回，stop 映射为结构化 `Error`。
- `evaluateActionTarget` 只接受 catalog 中的 `ActionTarget`，返回 `ActionTargetAttempt`；
  miss 是 `AnchorEvidence{hit=false}`，不是 error，stop 仍是显式 variant。

`RecognitionPolicy` 同时携带全局 pixel-comparison budget、可选 absolute deadline 与
`std::stop_token`。runtime 将 cancellation 和 deadline 按值捕获到 `SadSearchPoll`，再调用
`modules/vision/source/vision/sad.hpp` 的有界 overload。页面评估按 `pageAnchorOrder` 前进，并从剩余
budget 中扣除已完成 comparisons；任一 matcher 返回 `Cancelled`、`TimedOut` 或
`ComparisonBudgetExhausted` 时立即返回 stop，绝不把它解释为 forbidden-anchor miss。

frame 是 Gray8 时直接建立同步只读 view；是 Bgra8 时只转换一次并在 backing vector 活着期间调用 matcher。
`RecognitionRuntime` 自己拥有 manifest 和 decoded gray templates，因此外部传入的 encoded buffers
在 `create` 返回后不再承担生命周期。

`ensureCompatibleFrame` 在每次 page/action 识别前同时检查 live fingerprint 与 manifest fingerprint
相等，并检查 frame width/height 等于项目 base resolution。不匹配返回
`TargetCompatibilityUnverified`；S0 没有隐式 resampling。

### PageResolver、click pixel 与动作授权

页面证据类型在 `modules/annotation/source/annotation/recognition.hpp`：

- `AnchorEvidence` 记录 recognizer ID、hit、可选 SAD score、`maximumSad`、可选 matched rect 和展示 confidence。
- `PageEvaluation` 保存某 page 的 required/forbidden evidence 与 candidate 标志。
- `PageResolutionEvidence` 拥有 project ID、`FrameIdentity`、全部 page evaluations 和完整 candidate IDs。
- `ResolvedPage`、`UnknownPage`、`AmbiguousPages` 都拥有证据；只有 `ResolvedPage` 还能携带唯一 `PageId`。

`PageResolver::resolve` 要求输入覆盖 `pageAnchorOrder` 的每个 anchor 且顺序完全一致。它评估所有 page：
required 全 hit 且 forbidden 全 miss 才是 candidate。零个候选返回 `UnknownPage`，一个返回
`ResolvedPage`，多个返回 `AmbiguousPages`。它不会因为先遇到一个候选就提前成功，因此后面的冲突页面
不能被排序隐藏。

`resolveClickPixel` 位于
`modules/annotation/source/annotation/recognition-runtime.cpp`。对 `ActionTarget`：

- 有 `defaultClick` 时，用 checked addition 把模板局部 offset 加到 matched rect 原点；
- 没有 offset 时，取 `origin + extent / 2`，整数除法向下截断，从而为奇偶尺寸都选出唯一像素。

动作授权定义在 `modules/annotation/source/annotation/authorization.hpp` 和
`modules/annotation/source/annotation/authorization.cpp`。`ActionDetection::create` 不信任字符串 label：
它要求 recognizer 确实是 catalog 中的 `ActionTarget`，label 与该 recognizer name 相等，并把
`ProjectId`、`RecognizerId` 和拥有的 `Detection` 绑定在一个值中。

`authorizeCoordinateAction` 实现四条件 gate，并在每层继续做闭包校验：

1. **实时兼容条件**：`ActionDeliveryState::m_liveFingerprint` 必须等于 catalog fingerprint，否则
   `TargetCompatibilityUnverified`。
2. **同项目的 ResolvedPage 条件**：page evidence 和 action detection 都必须属于 active project；
   resolved page 必须仍存在于 active catalog。
3. **同帧 ActionDetection 条件**：recognizer 必须仍是 active catalog 的 `ActionTarget`，其
   `allowedPageIds` 必须包含 resolved page；page evidence、detection 和 delivery 的
   session/generation/frame 三元组必须完全相同。
4. **有效 ObservationLease 条件**：最后调用 `ObservationLease::validate`，用 delivery 时刻再次验证
   session、target generation、frame ID 和 expiry。

返回值只是 `Status`，没有“部分授权”。`UnknownPage` 和 `AmbiguousPages` 在类型上无法作为参数传入，
recognition stop 也不会生成 `ResolvedPage`，所以失败状态不会被误当成可点击能力。

## 设计不变量

**Fail-closed。** 所有外部数据都经 `Result` factory 构造，schema、资源闭包、hash、geometry、
fingerprint 或 quota 任一失败都不产生半有效对象。页面不唯一就没有 `ResolvedPage`；matcher stop
不是 miss；动作授权任一条件失败就没有 delivery permission。这是“宁可不点，也不在未知页面点错”
的结构化实现，不依赖调用者记住额外布尔判断。

**确定性。** 稳定 UUID 排序同时控制 canonical bytes、模板任务顺序、锚点评估顺序和候选顺序。
阈值使用 basis points 与 integer-inclusive SAD；默认点击使用整数 offset 或截断中心。模板 hash
覆盖实际 encoded PNG bytes，manifest path 从 hash 唯一派生。相同 document 与 source bytes
因而产生相同 manifest、template bytes、evidence ordering 和 click pixel。

**所有权与生命周期显式。** Document、catalog、manifest、runtime、evidence 和 action capability
都是 owning values；`RecognitionRuntime` 拥有解码后的模板。公开 span/pointer 是 catalog/document
内部存储的只读 borrow，不能越过 owner 生命周期。临时 `GrayImage` 只在同步 matcher 调用期间借用
frame 或局部转换 buffer；poll 捕获 token/deadline 的副本，不悬挂引用。`ActionDetection` 拥有
`Detection`，避免只保存可能碰撞的 label 或借用调用者状态。

**有界工作。** authoring/runtime TOML、每类记录、编译 pixels、生成 template bytes、PNG 输入和
SAD comparisons 都有显式上限。deadline 与 cancellation 和 comparison budget 一起进入相同 matcher
调用；这使 Preview、regression 和 Runtime 能共享停止语义，而不是在 UI 层另设不一致的超时规则。

**Strict-background 的证据链。** annotation 不接触输入 API，但它要求 page、detection、lease 与
delivery 指纹/身份同时成立。`modules/engine/source/engine/session.cpp` 在授权后还调用
`FrameSource::validateTargetInstance`，把 lease 传到 `ActionSink::click`，并在成功投递后使 observation
失效。因而 annotation gate 是两层投递围栏中的领域层，而不是完整后台协议的全部。

## 与其他部分的协作

入站 authoring 链路来自 `entry/workbench`：

- `source-ingestion.cpp` 将导入 PNG 或 WGC frame 变成 `AuthoringSourceSpec` 和
  `AuthoringSourceAsset`。
- `authoring-edit.cpp` 在 owning `AuthoringDocument` 快照上实现 edit/undo/redo。
- `preview.cpp` 调用 `compileAuthoringDocument`、`RecognitionRuntime::create`、
  `evaluatePage` 和可选 `evaluateActionTarget`，因此 Preview 没有私有 matcher。
- `project-persistence.cpp` 先完整编译，再发布 immutable source/templates，原子替换
  `annotations.toml`，最后以 `generated/annotations.runtime.toml` 作为 runtime commit point。

出站 runtime 链路进入 engine：

- `modules/engine/source/engine/runtime-loader.cpp` 读取 canonical runtime manifest 与其引用的唯一
  template PNG，交给 `RecognitionRuntime::create` 再验 hash closure。
- `modules/engine/source/engine/session.cpp` 从 config 构造 `RecognitionPolicy`，在同一个
  `Observation` frame 上 resolve page 和 find action。
- action hit 被转换为 domain `Detection`，再经 `ActionDetection::create` 绑定 recognizer identity；
  `resolveClickPixel` 生成 frame pixel，`authorizeCoordinateAction` 在 act 时验证四条件 gate。
- engine 把 pixel 转为 `FrameSpace`/`ClientSpace`，复验 target instance，才调用 controller-backed
  `ActionSink`。跨 annotation 边界传递的是值、证据和 `Status`，不是 OS handle。

`runAuthoringRegressions` 位于
`modules/annotation/source/annotation/regression-runner.cpp`。它先走真实 compiler/runtime 构造链路，
再逐个把 source PNG 构造成 frame 并调用 `evaluatePage`。comparison budget 每个 case 重启；
cancellation 与 absolute deadline 跨 suite。Cancel/timeout 中断后续 case，单 case budget exhaustion
作为该 case 的诊断保留并允许 suite 继续。

## 测试策略

`tests/annotation` 是确定性离线测试面；各文件固定的契约如下：

- `test-catalog.cpp`：UUID/name canonical form、basis-point 边界、recognizer geometry/page membership、
  空/重复/矛盾 signature、跨资源闭包与重复 signature。
- `test-authoring-document.cpp`：authoring 文档完整 byte-stable round trip、schema/顺序/整数/路径/
  RFC 3339/CRLF drift 拒绝，以及 regression classification 与 expectation 独立性。
- `test-runtime-manifest.cpp`：runtime-only 字段集、固定 writer bytes、string escapes、非 canonical
  数字、错误 template path、未知 kind/field、CRLF、尾部 comment 拒绝。
- `test-content-hash.cpp`：SHA-256 单块/多块标准向量和小写 canonical hash parser。
- `test-template-asset.cpp`：精确 crop、PNG encode、content address 与越界 crop 拒绝。
- `test-authoring-compiler.cpp`：save/reopen 后的确定性编译、source relationship、相同 crop/hash 去重、
  256 Mi-pixel 工作边界、缺失/篡改 source closure 和解码 geometry。
- `test-recognition.cpp`：`sadScore == maxSad` 命中、Resolved/Unknown/Ambiguous 无优先级，以及三种
  matcher stop 都终止完整 page resolution。
- `test-recognition-runtime.cpp`：Gray8/Bgra8 evidence 等价、page outcome、全局 budget、cancel、
  deadline、fingerprint/template closure、共享模板、action hit/miss/type checks、action stop 与
  `resolveClickPixel`。
- `test-authorization.cpp`：fingerprint、同帧 identity、recognizer-label binding、allowed page、
  active catalog/project 和 lease 组成的授权门。
- `test-regression-runner.cpp`：resolved/unknown/ambiguous evidence、expectation mismatch、
  suite interruption、per-case budget 与 source 复用。
- `test-helpers.hpp` 只提供强类型 fixture/builders，不是另一套生产逻辑。

修改 schema writer/parser、排序、阈值或 stop 传播时，至少应同时检查“构造 validation 测试”和“完整
compiler/runtime 测试”。只增加 parser 单测不足以证明 Preview 与 Runtime 仍走同一路径；只增加
runtime happy path 也不足以证明非 canonical 输入继续 fail-closed。

## 扩展接缝

以下接缝来自权威计划，不是当前已实现能力。

**P1 分辨率适配。** `docs/plans/2026-07-22-annotation-design.md` §2 和
`docs/plans/2026-07-21-lua-task-model-grill-decisions.md` D8 要求新增显式
`BaseToLiveTransform { uniformScale, offset, viewport }`。它应位于 base annotation geometry 进入
live frame search/click geometry 的边界，并完整进入 trace；不能修改现有 live
`CoordinateTransform` 的 Client↔Frame 职责，也不能把裸 scale 散入 recognizer。

**新 recognizer kind。** `RecognizerDefinition` 已把 type、geometry、threshold 和 page membership
集中，`RecognitionRuntime` 又把已解码模板隔离为内部 `GrayTemplate`。新增颜色、OCR 或 composite
时，接缝会贯穿 schema version/parser、catalog validation、compiler asset closure、runtime-owned
kernel 与 evidence；不能只在 workbench 增加一个 UI 选项。权威计划 §7 明确 P0 只允许
`gray_template`，OCR 只有在真实日常必须读取动态语义且模板/状态锚点无法表达时才能提前裁决。

**`InfoRegion` 求值。** enum 和 authoring/runtime schema 已能表达 `InfoRegion`，但当前
`AnchorEvaluation::fromSadOutcome` 只接受 `PageAnchor`/`ActionTarget`，公开 runtime 也没有
`evaluateInfoRegion`。未来读取型 API 应在 `RecognitionRuntime` 增加独立结果类型，复用相同
policy、hash closure 和 evidence 规则，而不是伪装成可授权 action。

**页面诊断而非启发式消歧。** `PageResolutionEvidence` 已保留全部 page 与 candidate evidence，
可支撑 `docs/plans/2026-07-21-product-form-and-roadmap.md` P1 的 page confusion 诊断。扩展应消费这份
证据改善 authoring 体验；S0 权威禁止在 runtime 加 priority、threshold override 或 heuristic
tie-break，所以诊断不能改变 `Ambiguous` 的 fail-closed 语义。

**P1/P2 host surface。** Luau opaque handles、popup interrupt、P2 no-activate overlay 和 HTML trace
都在 annotation 之外。它们应消费 `RecognitionCatalog` 的稳定 IDs/names、
`PageResolutionEvidence`、`AnchorEvidence` 和 stop reason。尤其 overlay 可以展示 evidence，
但不能成为新的 matcher 或绕过 `authorizeCoordinateAction`。

任何改变 canonical bytes、schema 字段、threshold 决策、page outcome 或四条件授权门的工作，都不是
局部重构：它改变 S0 shared contract。实施前应先更新
`docs/plans/2026-07-22-annotation-design.md` 的权威决策，再同步 authoring writer/parser、runtime
loader、workbench Preview、engine integration 和上述测试矩阵。
