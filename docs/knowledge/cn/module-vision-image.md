# vision 与 image 模块架构知识

本文合并说明两个小型平台无关模块：`modules/vision` 提供确定性的像素识别，
`modules/image` 负责有资源上限的 PNG 编解码和像素布局。编辑器、预览和运行时都会
使用它们，但两个模块互不依赖。相关 S0 设计见
`docs/plans/2026-07-22-annotation-design.md`。

## 模块分工

`vision` 拥有五件事：

1. 将带 stride 的 BGRA8 frame 确定性转换成紧密排列的 Gray8 bytes。
2. 在整数 `PixelRect` 内执行 SAD（sum of absolute differences）模板搜索，并给出稳定的最佳位置与
   `uint64` score；还可以**按一张与模板同形的 mask 平面给每个模板像素加权**
   （2026-07-30，`c392161`）。
3. 为长搜索提供精确 comparison budget、同步 cancellation/deadline poll 和不丢失的停止原因。
4. 拥有色键权重规则：`modules/vision/source/vision/frame-analysis.hpp` 的 `colourKeyAlpha` 是它的
   唯一实现，`annotation::ColourKey::alphaFor` 转调到这里。
5. 在 `modules/vision/source/vision/frame-analysis.hpp` 里回答关于**一组**已解码帧的三个测量问题
   ——稳定性、色键探针、颜色普查（2026-07-30，`eacb05f`）。

`image` 拥有另外三件事：

1. 在 `uf::image` 命名空间中把 PNG bytes 或文件解码成自有、紧密排列的 RGBA8 pixels，并执行反向编码。
2. 显式转换 RGBA8/BGRA8 布局，以及从带 stride 的 BGRA8 source 确定性裁出紧密排列的矩形。
3. 把 stb 的 C API、foreign allocation、raw pointer 和 process-global encoder 配置封闭在
   `modules/image/source/image/ffi/`。

两个 `manifest.txt` 都公开依赖 `core` 和 `domain`；`image` 另有私有的 `image_stb` dependency，
见 `modules/vision/manifest.txt`、`modules/image/manifest.txt` 和
`modules/image/external/CMakeLists.txt`。模块图没有 `vision -> image` 或 `image -> vision`：

- `vision` 不需要知道静态模板来自 PNG、WGC 还是测试 fixture，只接收已经验证的像素 view。
- `image` 不需要知道 pixels 将用于识别、画布纹理还是 capture evidence，只做 codec 与布局工作。
- 需要“PNG → Gray8”时，`annotation` 或 entry 层显式组合
  `decodePng`、`rgba8ToBgra8`、`bgra8ToGray8`。这个显式桥接避免 codec/FFI 污染纯识别核。

它们不负责以下策略：

- 不发现窗口、不抓帧、不持有 OS handle，也不发送 `PostMessage`；这些属于 controller 与 entry adapter。
- 不定义相似度阈值、page signature、required/forbidden anchor、`ResolvedPage` 或动作授权；这些属于
  `annotation`。
- 不计算 `ContentHash`，不派生 `assets/templates/<hash>.png`，也不校验 runtime asset closure。
  `image` 只保证编码 bytes；`annotation` 决定这些 bytes 的身份含义。
- 不原子发布 authoring project。`writeRgbaPng` 是直接 open/truncate/write/flush/close 的文件函数；
  commit point 和回滚纪律在 `entry/workbench/project-persistence.cpp`。
- 不做缩放、resampling、颜色 recognizer、OCR、composite recognizer 或多尺度搜索。
- 不实现 strict-background input。这里对该产品契约的贡献是：无效图像被拒绝，未完成搜索保留为 stop，
  从而上层没有条件把不完整证据变成一次后台输入。

## 图像处理与识别流程

### vision 的公开接口

公开声明位于 `modules/vision/source/vision/sad.hpp`。

`GrayImage` 是只读 Gray8 view，不拥有 pixels。`GrayImage::create` 接受 span、`width`、`height` 和
`stride`，复用 `modules/domain/source/domain/frame.cpp` 的 `validateBufferGeometry` 拒绝零尺寸、短
stride、乘法溢出或不足的 backing storage。成功后只保存 span 与 geometry，因此 backing owner 必须覆盖
view 及所有同步 matcher call 的生命周期。

`bgra8ToGray8` 接受同样的带 stride geometry，验证每行至少为 `width * 4` bytes，然后逐像素执行：

```text
gray = (77 * red + 150 * green + 29 * blue) >> 8
```

实现位于 `modules/vision/source/vision/sad.cpp`。它忽略 alpha，跳过输入 row padding，返回恰好
`width * height` bytes 的 owning vector。整数权重和右移固定了截断行为；例如纯红、绿、蓝分别得到
76、149、28，而不是依赖平台浮点或颜色库的 rounding。

`SadMatch` 保存 `x`、`y` 和 `score`。score 越小越好，零表示逐像素完全相同。`vision` 本身没有
threshold：只要 template 能在 ROI 中合法放置，completed search 就返回最佳 `SadMatch`；
`std::optional<SadMatch>` 为空只表示没有合法候选，不表示“score 超过业务阈值”。

三种 control 类型都在同一头文件中：

- `SadSearchControl` 是 poll 的返回值：`Continue`、`Cancelled`、`TimedOut`。
- `SadSearchStopReason` 是 matcher 对外保存的停止结果：`Cancelled`、`TimedOut`、
  `ComparisonBudgetExhausted`。
- `SadSearchPoll` 是同步调用且不会被 matcher retain 的 `std::function<SadSearchControl()>`。

`SadSearchOutcome` 区分 `std::optional<SadMatch>` 与 `SadSearchStopReason`。`SadSearchReport` 再附带
`m_completedPixelComparisons`；计数覆盖所有实际比较，包括触发 pruning 或 exact-match return 的那一次，
但不包含 budget/poll 已阻止的下一次比较。

`matchTemplateSad` 有四个 overload，两个不带 mask、两个带 mask：

- 三参数 overload 返回 `Result<std::optional<SadMatch>>`，内部使用最大 `uint64` budget 和永远
  `Continue` 的 poll。
- 有界 overload 接受 `maximumPixelComparisons` 与 `SadSearchPoll`，返回
  `Result<SadSearchReport>`。Runtime、Preview 与冻结的 m0-demo 都使用这个 overload。
- 另两个 overload 在模板与 ROI 之间多收一个 `templateMask`，分别对应上面两种形状。它们是
  **增量的**：现有每一处调用都没被动过。

#### 带 mask 的匹配器

mask 是一张与模板范围完全相同的 Gray8 平面。mask 字节 255 表示该像素全额计入，0 表示排除，
中间值是**部分权重**——这正是抗锯齿字形边缘需要的东西。报出的 score 按实际累加到的权重归一，
再放大回模板的完整矩形：

```text
score = templatePixels * sum(weight * |haystack - template|) / sum(weight)
```

这次放大回去才是要点。只盖住十分之一矩形的 mask 与盖满全部的 mask 处在同一量纲上，
因此也就处在既有无 mask 阈值本来使用的量纲上，`SimilarityThreshold` 不必学会 mask 这回事。
除法截断向下取整。**权重和为零的 mask 什么都没选中，被拒绝而不是拿去做除数。**

有两条性质守住旧行为。overload 是增量的，现有调用一处未改；而且**全不透明的 mask 与无 mask
匹配器逐位一致**——同样的分数、同样的命中、同样的剪枝决定、同样的比较次数——因为常数 255
在商里和每一次剪枝比较里都被约掉了。验证方式是把每一个模板都强行走一遍带 mask 的路径，
看着什么都没变。

颜色不是匹配器，也绝不能是。实测那份菜单：亮白色占菜单区域的 8.2%，却占背后画面的
10.9%–18.7%——只按颜色判定，角色立绘会压过菜单。**颜色选的是拿哪些像素来比，认形状的仍是 SAD。**

`bgra8ToAlpha8` 把 BGRA8 缓冲的 alpha 通道抽成一张紧密排列的 Gray8 形状平面，那就是带 mask
匹配器消费的 mask。模板 PNG 的 alpha 通道*就是*这张平面，这也是色键不需要新资产类型的原因
——见 `module-annotation.md`。

#### 帧分析

`modules/vision/source/vision/frame-analysis.hpp` 回答关于一组帧的三个问题，每一个都被手工
答过一次，也都慢到不值得再手工答第二次。帧以已解码的 `BgraImage`（BGRA8 平面，
`modules/vision/source/vision/bgra-image.hpp`）进来；这个模块从不碰文件。

**它们收 N 帧，不是两帧。** `analyseStability` 与 `probeColour` 各收一个帧的 `std::span` 而不是
一对，因为有用的那一组是两张不同背景，*外加一张几秒后再拍的*用来抓动画：一个像素在前两张里
稳定、在第三张里动了，它就不稳定；而这个差别对语义和肉眼都不可见，却会制造时有时无的匹配失败。
两帧的 API 一落地就得换掉。两者都要求至少两帧——一帧到处都稳定，什么也答不了。

- `analyseStability(frames, StabilitySpec)` 报出一个 rect 里哪些像素在每一帧上都没动。它的
  `stableMask` 在所有帧一致处是 255、其余是 0，形状就是可以直接交给 `matchTemplateSad` 当模板
  mask 的形状。`grayTolerance` 是一个像素仍可被称为稳定的灰度跨度，零表示要求逐字节相同——
  这正是画在变化画面上的 UI 实际产生的结果。`minimumGap` 按一段完全不稳定的连续行或列把区域
  切开；零关闭切分，报出那个朴素的单一包围盒，而它会把菜单标签和它下面的子标签并成一块。
  它还返回切区域所用的行、列剖面，让调用方照数据挑 `minimumGap`，而不是靠猜。
- `probeColour(frames, ColourProbeSpec)` 测量一个色键把 rect 里「不动的东西」隔离得有多好。
  它报出 `fullySelectedPixels` 与 `rampSelectedPixels`，而 `maskedMeanGraySpread` 与
  `rectMeanGraySpread` 之间的落差，就是「这个 rect 扛不扛得住换背景」的全部答案。
  `fullySelectedPixels` 同时是防住「一个什么都选不中的键」的唯一守卫：编辑期没有这项检查。
- `censusColours(frame, ColourCensusSpec)` 数**一**帧的一个 rect 用了哪些颜色，让色键从数据里
  挑出来，而不是采一个像素然后祈祷。alpha 被忽略——捕获帧的 alpha 不携带颜色。它是单帧且
  O(像素) 的，所以不像上面两个扫描那样收 budget。计数相同的项按打包后的 blue/green/red 升序
  排列，因此报告永远不依赖遍历顺序。

两个扫描共用匹配器的 budget 与取消词汇——`SadSearchPoll`、`SadSearchControl`、
`SadSearchStopReason`、`k_sadSearchPollIntervalComparisons`——而不是另立一套，并且按匹配器数
比较次数的方式数 `completedPixelVisits`（每帧每像素一次）。那些名字上的 `Sad` 前缀是历史遗留；
一个模块一套约定，比一个更好的前缀值钱。

`colourKeyAlpha(pixel, keyRed, keyGreen, keyBlue, tolerance)` 是色键权重规则的**唯一**实现：
在容差以内全额，然后线性斜坡到两倍容差处归零，容差为零则退化成精确颜色匹配。距离是三个通道
距离之和，所以 `k_maximumColourKeyTolerance == 765` 是仍然有意义的最大容差。它住在这里而不是
`annotation`，是因为 `probeColour` 需要它来回答「这个键选中了多少像素」——那正是那个函数存在的
理由；`annotation::ColourKey::alphaFor` 转调过来，依赖方向是 annotation → vision，所以调用只能
朝这个方向走。2026-07-30（`eacb05f`）之前这条规则有两份逐字节相同的实现，而那种走偏是无声的：
编辑期烘出一张 mask，探针却报出另一张。

有界搜索先调用 `roi.ensureWithinExtent`。合法候选按 `candidateY` 外层、`candidateX` 内层的 row-major
顺序遍历并逐行累加绝对差。差值非负，所以一行后 partial sum 已 `>= best` 时可以安全 pruning。只有
`score < best` 才替换结果，因此 tie 保留最早位置；零分立即返回，因为不存在更优 score。

budget 与 poll 都在“执行下一次 pixel comparison 之前”检查。顺序是先检查 budget，再在累计计数为
`0, 4096, 8192, ...` 时 poll；间隔常量是
`k_sadSearchPollIntervalComparisons == 4096`。因此：

- budget 为零时直接返回 `ComparisonBudgetExhausted`，不调用 poll，计数为零；
- poll 在第一次比较前就能报告 immediate cancel/timeout；
- 已执行恰好 4096 次后，若仍有 budget，下一次比较前会再次 poll；
- budget 刚好耗尽时，stop 不会把未执行的比较算入报告。

`modules/vision/source/vision/synthetic.hpp` 另公开 `hashedGray`。它用固定的 unsigned integer
multiply/xor 序列从 `seed`、`x`、`y` 生成一个 `uint8`，当前调用者是
`tests/vision/test-sad.cpp` 的确定性合成 fixture；它不是 PNG hash 或内容身份算法。

### image 的公开接口

PNG API 位于 `modules/image/source/image/png.hpp`。三个公开 quota 是：

- `k_maximumPngDimension == 8192`：每个轴的上限；
- `k_maximumPngPixels == 8192 * 8192`：总 pixel 上限；
- `k_maximumPngFileBytes == 64 * 1024 * 1024`：encoded decode/load 输入上限。

`RgbaImage` 拥有 `m_width`、`m_height` 和 `m_pixels`。由 decoder 返回时，pixels 总是
`width * height * 4` 的紧密 RGBA8，PNG 原始 color type 不泄漏到调用者。

`decodePng` 的输入是同步借用的 encoded span 和只用于诊断的 `resourceName`。实现在
`modules/image/source/image/ffi/png-decoder.cpp`，进入 stb 前依次执行：

1. 拒绝空输入和超过 64 MiB 的 encoded bytes。
2. `validatePngStructure` 检查 PNG signature、首 chunk 必须是长度 13 的 IHDR、不得重复 IHDR、
   每个 declared chunk length 都落在输入内、IEND 必须为空且正好结束文件。
3. 从 IHDR 预读 width、height、bit depth，拒绝零尺寸、轴 quota、pixel quota 与 checked-size 溢出。
4. 预先分配精确 RGBA8 destination，再把受界 span 交给 stb。

decoder 定义 `STBI_ONLY_PNG`、`STBI_NO_STDIO` 和 `STBI_NO_FAILURE_STRINGS`。普通输入调用
`stbi_load_from_memory` 并强制 `STBI_rgb_alpha`；16-bit PNG 调用 `stbi_load_16_from_memory`，每个
sample 通过 `(sample * 255 + 32767) / 65535` round-to-nearest。foreign allocation 立即进入带
`Stbi8ImageDeleter` 或 `Stbi16ImageDeleter` 的 `std::unique_ptr`，stb pointer 不越过函数边界。

`loadPng` 先用 `std::filesystem::file_size` 执行 64 MiB preflight，再精确读取 bytes 并调用
`decodePng`。缺失、不可读、短读或非法模板都返回 `InvalidResource`。当前 parser 与 stb 不验证 PNG
chunk CRC；这是 `docs/plans/2026-07-20-m0-demo-port-deviations.md` F-14 对“trusted template input”的
明确保留边界，不能把结构预检描述成完整的 cryptographic integrity check。

`encodeRgbaPng` 和 `writeRgbaPng` 实现在
`modules/image/source/image/ffi/png-encoder.cpp`。encoder 拒绝零尺寸、轴/pixel quota 超限、stb signed
geometry 不可表示，以及不等于 `width * height * 4` 的非紧密 RGBA input。它还用私有的
`k_maximumFilteredPngBytes` 约束 `(rowBytes + filterByte) * height`，并用 `static_assert` 证明该上限
远低于 stb 的 signed working range。64 MiB quota 是 decode/load encoded-input 上限；encoder 自身的
本地工作上限由 dimension、pixel 和 filtered-byte checks 构成。

编码不调用 stb 文件 I/O，而是用 `STBI_WRITE_NO_STDIO` 和 `stbi_write_png_to_func`。
`appendEncodedPng` 把每段 callback bytes 追加到每次调用独有的 `EncodedPng::m_bytes`。callback 是
`noexcept`，对 null、非正 size、checked-size overflow 或 allocation exception 只设置
`m_callbackFailed`；C callback boundary 上没有 exception 逃逸。stb 的 `STBIW_ASSERT` 被替换为
release-active `UF_CHECK_MSG`，使 stretchy-buffer realloc failure 在继续写越界前终止。

第一次编码前，function-local magic static 明确写入：

```text
stbi_write_png_compression_level = 8
stbi_write_force_png_filter = -1
```

这两个值是被显式冻结的配置，而不是从 mutable process defaults 偶然继承。static initialization guard
让写入只发生一次，并使并发 caller 在 stb 读取这些 non-atomic globals 前完成同步。仓库中没有其他 setter；
`tests/image/test-png.cpp` 的完整 golden byte sequence 则在 stb 实现升级改变输出时强制人工审查。

`writeRgbaPng` 复用 `encodeRgbaPng` 后逐步检查 open、write、flush、close。文件系统失败返回
`IoFailure` 并保留 native `std::error_code`；stb/callback 编码失败返回 `ExternalFailure`。

像素 API 位于 `modules/image/source/image/pixels.hpp`：

- `rgba8ToBgra8` 与 `bgra8ToRgba8` 都按值接收 vector，并在原 allocation 上交换每个四字节 pixel 的
  red/blue；green 与 alpha 不变，byte count 不是四的倍数时返回 `InvalidResource`。
- `cropBgra8` 接受 borrowed source、source geometry、stride 与 `PixelRect`。它检查 rect 边界、
  row/storage geometry 和全部 checked byte offsets，逐行复制有效 BGRA bytes，返回无 padding 的
  `rect.width() * rect.height() * 4` owning vector。

### 两条实际数据链

authoring 资产链把“静态裁剪”和“运行时搜索范围”保持为两个不同操作：

```text
source BGRA8 + templateRect
  -> image::cropBgra8 -> image::bgra8ToRgba8 -> image::encodeRgbaPng
  -> annotation::sha256(encoded PNG bytes) -> content-addressed template
```

runtime/Preview 识别链则为：

```text
template PNG -> image::decodePng -> image::rgba8ToBgra8 -> vision::bgra8ToGray8
live Frame BGRA8 -----------------------------------------> vision::bgra8ToGray8
two GrayImage views + independent searchRoi -> bounded matchTemplateSad
```

模板 PNG hash 覆盖 encoded bytes，而不是 decoded pixels。因此相同 pixels 若由不同 encoder/config
产生不同 PNG bytes，就会得到不同 identity；pinned encoder 与 golden test 是内容寻址契约的一部分，
并非单纯的压缩性能设置。

## 必须保持的约束

**确定性。** Gray conversion 只有固定宽度整数运算；SAD 只有整数绝对差与 `uint64` 累加；candidate
顺序、strict-less update、row pruning 和 exact-zero return 固定 tie 结果。channel swap、crop 与
16→8 bit conversion 也有唯一 byte result。PNG 层通过显式 stb globals、每次调用独立 output buffer
和 golden bytes 把相同 RGBA input 收敛到同一 encoded sequence。任何这些规则的变化都会影响 evidence、
trace 或 content hash，不能当作无语义重构。

**Fail-closed。** geometry、stride、buffer size、ROI、checked arithmetic、PNG structure 和 quota 都在
读取或 foreign allocation 前验证。FFI callback 无法安全追加时只产生失败，不返回部分 PNG；
foreign decode pointer 为空或 dimensions 与预检 IHDR 不一致时不返回 image。内部“不可能”状态使用
release-active `UF_CHECK`，外部资源错误使用 `Result`。唯一明确例外是当前 trusted-input CRC 决策，
它由保留计划记录，不能被误写成已经验证。

**Search stop 不是 miss。** `Cancelled`、`TimedOut` 和 `ComparisonBudgetExhausted` 都表示没有完成所有
必要候选，因而既不能证明“存在最佳 match”，也不能证明“没有可接受 match”。若把 stop 折叠成
`hit=false`，required anchor 会错误地产生 Unknown，更危险的是 forbidden anchor 会因“未命中”而帮助
页面成为 candidate。`SadSearchOutcome` 的独立 variant alternative 从 kernel 开始阻止这种信息丢失；
`annotation` 再将三者分别映射到 `Cancelled`、`Timeout`、`RecognitionIncomplete`。最后这个 kind 取名
取的是“没看完”，不是“没认出来”：它表示搜索根本没看完，所以 `FailureResponse` 是 `Retry`，
调用方必须重新观察，而不能对屏幕下任何结论。

**所有权与生命周期显式。** `GrayImage` 是短生命周期 borrow，声明与实现都不保存 backing owner；
matcher 与 poll 都同步完成且不 retain callback。PNG decode/encode result、crop 和 layout conversion
result 都是 owning vector。FFI allocation 用 custom-deleter `unique_ptr`，raw pointer 只在带
`// SAFETY:` 证明的同步 copy/view 中出现。上层 `RecognitionRuntime` 最终拥有 decoded Gray8 template，
所以原始 PNG 和中间 RGBA/BGRA buffers 可以在创建后释放。

**工作量有界且可记账。** PNG 在调用 stb 前限制 encoded bytes、axes、pixels 和 destination size；
encoder 另限制 filtered working bytes。SAD 的 product path 同时有 comparison budget、每 4096 次的
cooperative poll 和精确 completed count。pruning 是性能优化，但不放松 budget/poll，也不改变最佳结果。

**Strict-background 只通过证据链间接成立。** 这两个模块没有 input capability，也不能承诺窗口后台投递。
它们保证交给上层的是 completed evidence 或显式 stop。`modules/engine/source/engine/session.cpp` 只有在
annotation 产出可授权的 completed evidence 后才可能调用 `IActionSink::click`；stop 会先进入 trace 并
返回 error。因此这里守住的是 strict-background 的识别前置条件，而不是 delivery protocol 本身。

## 依赖关系

向下依赖中，`core` 提供 `Result`、release-active contracts、checked arithmetic/casts 和 checked
access；`domain` 提供 `PixelRect`、`PixelFormat`、`Frame` 与 `validateBufferGeometry`。跨这条边的只有
整数 geometry、span/vector 和结构化 error，没有 stb 类型或平台 handle。

`modules/annotation` 是两模块最重要的共同 consumer：

- `modules/annotation/source/annotation/template-asset.cpp` 调用 crop、BGRA→RGBA、PNG encode，再对
  encoded bytes 做 SHA-256，生成 `TemplateAsset`。
- `modules/annotation/source/annotation/recognition-runtime.cpp` 在
  `RecognitionRuntime::create` 中复验 template hash closure，decode PNG，RGBA→BGRA→Gray8，并拥有最终
  gray template。
- 同一文件的 `withGrayFrame` 对 Gray8 `Frame` 直接建立 view，对 BGRA8 `Frame` 只转换一次，并保证局部
  gray vector 活到 continuation 返回。
- `RecognitionPolicy` 被转换成按值捕获 stop token 与 deadline 的 `SadSearchPoll`；page evaluation
  还在多个 anchor 之间扣减同一 global comparison budget。

`entry/workbench` 直接消费 `image`：

- `source-ingestion.cpp` 将导入 PNG decode 后重新 canonical encode，或将 WGC BGRA frame 去 padding、
  转 RGBA 后编码；source hash 因而覆盖项目自己的 canonical PNG bytes。
- `preview.cpp` 复用正式的编译器和运行时路径，不维护私有 matcher。
- `platform/windows-texture-cache.cpp` 只把 `decodePng` 的 RGBA output 交给 D3D texture upload。
- `project-persistence.cpp` 才拥有文件发布顺序和 encoded asset size gate。

`modules/engine` 不直接依赖 `image` 或 `vision`；它经公开依赖 `annotation` 获得 recognition result，
并在 `modules/engine/source/engine/trace.cpp` 序列化 `SadSearchStopReason`。这使 engine 看到
recognizer evidence 和 stop vocabulary，却看不到 codec 或 matcher 内部 storage。

`entry/m0-demo` 是冻结的真机验收参考，仍直接使用两模块：load/convert template、crop/convert frame、
bounded SAD，以及 capture PNG 输出。它的直接调用不应被当作新产品策略的扩展点；当前产品路径是
annotation + engine。

controller 只生产带 `PixelFormat`、stride 与 owning `FrameBuffer` 的 `Frame`。它无需链接任一模块；
composition 层把 frame 交给 recognition。反向也没有 edge：识别核绝不调用 capture 或 input。

## 测试

`tests/vision/test-sad.cpp` 是 `test-vision` 的完整直接测试面，固定：

- exact hit、与 brute-force exhaustive scan 一致、row-major tie、padded stride 和 ROI 精确边界；
- zero/one/exact comparison budget、exact-match early return 的计数；
- `Cancelled`、`TimedOut`、`ComparisonBudgetExhausted` 三种 stop 与 4096 comparison poll interval；
- 非法 `GrayImage` geometry、越界 ROI 的 error kind；
- BT.601 integer samples、alpha ignored、input padding ignored、tight Gray8 output 与坏 geometry 拒绝；
- 带 mask 的匹配器：全不透明 mask 精确复现无 mask 的结果、权重和为零的 mask 被拒绝而不是拿去做
  除数，以及帧分析原语对 `tests/vision/label-fixture.hpp` 的验证——那是真实菜单标签在两种背景下
  的像素。其中一个数字是硬交叉校验而不是「结果吻合」：该 fixture rect 上的总和与带 mask SAD
  测试钉住的 `opaqueScore` 是同一个数，因此两份 fixture 可证明是同一个矩形。

`tests/annotation/test-colour-key-join.cpp` 是两半之间的接合测试。这两半是并行做出来的，在它
存在之前各自只对着手工构造的产物测过自己那一端。它把 `compileAuthoringDocument` 吐出的 PNG
字节直接送进 `RecognitionRuntime`，中间没有任何手工构造：带色键时该标签以 0.046 的平均差命中，
不带色键时同一个矩形以 56.979 对 25.5 的阈值落空。证伪方式是把色键指向画面，mask 随之反转，
平均差变成 69.47。

`tests/image/test-pixels.cpp` 固定 RGBA/BGRA byte order、incomplete pixel 拒绝、template/frame 共用同一个
gray kernel，以及带 padding crop 的紧密输出与短 source 拒绝。

`tests/image/test-png.cpp` 固定：

- `writeRgbaPng` → `loadPng` 的精确 RGBA round trip；
- 相同输入重复编码 byte-identical；
- 一个 2×2 fixture 的完整 PNG golden sequence，覆盖 header、IDAT、CRC bytes 与 IEND；
- write failure 的 `IoFailure`、native error category 和 operation message；
- encoder/decoder dimension quota、空/非 PNG、超限 IHDR、truncated chunk length；
- 16-bit sample 的 round-to-nearest RGBA8 结果。

跨模块 contract 由以下测试继续钉住：

- `tests/annotation/test-template-asset.cpp` 固定 stride-aware crop、channel order、重复编码 bytes、具体
  SHA-256 与 hash-derived path；encoder bytes 一变，template identity 测试会直接失败。
- `tests/annotation/test-authoring-compiler.cpp` 固定 deterministic compilation、相同 crop/hash 去重、
  pixel-work 边界、source hash/geometry closure。
- `tests/annotation/test-recognition.cpp` 固定 inclusive integer threshold，并证明每个 matcher stop 都终止
  page resolution。
- `tests/annotation/test-recognition-runtime.cpp` 固定 Gray8/BGRA8 evidence 等价、跨 anchor global budget、
  immediate cancel、expired deadline、template decode/hash closure 和 action-target stop。
- `tests/annotation/test-regression-runner.cpp` 固定 cancel/timeout 对 suite 的中断，以及 budget stop 作为
  per-case diagnostic 的传播。
- `tests/workbench/test-source-ingestion.cpp` 固定 import canonical re-encode、WGC provenance、BGRA/RGBA
  bridge 与 stride padding removal；`tests/workbench/test-preview.cpp` 固定 Preview 暴露 budget stop。
- `tests/engine/test-session.cpp` 固定 budget/cancel stop 为 error、零 click 和 `RecognitionStopped` trace；
  `tests/engine/test-trace.cpp` 固定 stop reason 的 JSONL spelling。

`tests/CMakeLists.txt` 注册 CI-labeled `test-vision` 与 `test-image`，并把 integration 放进各自 target。
`tests/workbench/test-real-regression.cpp` 只有本地 `tests/assets/real-regression` 存在时才注册为
`REAL`，用于未来真实截图而不把游戏资产放入 CI。

改动 matcher traversal/pruning/poll 时，不能只验证最终坐标，还要验证 exact comparison count 与 stop
时点；改动 codec/config 时，不能只做 decode round trip，还要验证 golden bytes、template SHA-256 和
authoring compiler determinism。前者保护控制语义，后者保护内容身份。

## 后续扩展

`docs/plans/2026-07-22-annotation-design.md` §7 锁定 P0 只有 bounded deterministic
`gray_template`。颜色、HSV、OCR、composite、parameterized ROI 和 multi-scale 都不是当前 kernel 的
隐藏模式。若权威计划允许新 recognizer，它应新增并列、同样有界的 kernel/result contract，并同步
annotation schema、asset closure、evidence、Preview/Runtime 与 trace；不能把新语义塞进
`matchTemplateSad` 后仍沿用旧 stop/score 含义。

同一权威的 §2 与 `docs/plans/2026-07-21-product-form-and-roadmap.md` P1 预留分辨率适配：计划中的
`BaseToLiveTransform { uniformScale, offset, viewport }` 和 deterministic template resampling 必须是
base annotation geometry 到 live frame geometry 之间的显式步骤。当前代码并不存在该类型；
接入点在 annotation runtime 组装 live ROI/template 与 `GrayImage` 之前，而不是悄悄改变现有
`CoordinateTransform` 的 Client↔Frame 职责，也不是让 `image` decoder 猜测 DPI。

OCR 只有在真实日常必须读取动态语义文字/数字且模板或 state anchor 无法表达时才允许重新裁决，见
`docs/plans/2026-07-22-annotation-design.md` §7 和 product roadmap P1。届时 PNG ingress 与 quota
仍可复用，但 OCR output、determinism、budget/cancellation 和 evidence type 都需要独立设计；SAD score
不是通用 confidence 接口。

升级 stb 或替换 codec 时，只需修改两个 `ffi/*.cpp` 和私有 `image_stb` target，公共
`image/png.hpp` 不必泄漏第三方类型。但 `docs/plans/2026-07-23-engine-architecture.md` Phase 1 明确指出，
encoder 配置必须在真实资产前冻结，因为 bytes drift 会作废全部 `template_hash`。任何升级都应先解释
`tests/image/test-png.cpp` golden diff，再决定是否迁移并重生 content-addressed source/template assets；
不能只更新 golden 常量。

当前 CRC 取舍依赖 trusted template。若未来产品开始接收下载或共享的不可信资产，应先重新裁决
`docs/plans/2026-07-20-m0-demo-port-deviations.md` F-14，并在 FFI preflight/integrity 边界增加相应验证，
而不是让每个 annotation/engine caller 各自补检查。

真实截图回归可接入 `tests/workbench/test-real-regression.cpp` 和条件注册的
`tests/assets/real-regression`。`docs/plans/2026-07-23-engine-architecture.md` Phase 5 要求用 workbench
产出的真实 source 扩展正例、负例与易混淆集；kernel 与 codec 不应包含任何游戏特判。

最后，改变 Gray formula、SAD traversal/tie、poll interval、PNG canonical bytes、quota 或 pixel layout
都可能跨越 S0 shared contract。实施前应先检查并在需要时更新
`docs/plans/2026-07-22-annotation-design.md`，随后同步 vision/image 直接测试、annotation content hash 与
recognition tests、workbench Preview，以及 engine stop trace；这些不是可以孤立发布的局部实现细节。
