# UmbraFlow 产品形态与 Roadmap(脱离旧 DESIGN 重锚)

> 状态:进行中,2026-07-21 grill 敲定方向层。这份是**产品方向权威**,取代旧
> `/e/github/UmbraFlow/DESIGN.md` 的里程碑 M0–M4。实现层裁决见
> [`2026-07-21-lua-task-model-grill-decisions.md`](2026-07-21-lua-task-model-grill-decisions.md)(D0–D10,存款性质)。
> 决策弹药见 [`2026-07-21-lua-task-model-decision-package.md`](../archive/plans/2026-07-21-lua-task-model-decision-package.md)。

## 背景:为什么重锚

Rust→C++ 移植已收尾。开发者决定**脱离旧 DESIGN.md 约束**,从手头信息 + 调研,自上而下
重新锚定产品形态与方向。顺序:**产品最终形态 → 方向/差异化 → roadmap → 实现细节最后回填**。

## 一、产品定义

> **UmbraFlow 是一个严格后台的个人游戏挂机 App:用脚本几分钟写个日常任务,托盘里点一下,
> 它就在你正常用电脑的同时替你把游戏日常刷完,并留下可回放的证据。**

三根支柱:
- **后台**:严格后台运行,永不抢焦点、不全局注入。用电脑同时挂机。
- **脚本化**:不重编译,几分钟写个新日常。脚本语言已定 Luau(见第五节)。
- **可回放**:确定性 + trace,是"无人值守挂机"敢用的信任地基,非学术洁癖。

## 二、差异化

> 对比口径:按 2026-07-21 的公开定位与能力比较;UmbraFlow 列写的是**产品契约/目标**,
> 不表示 Roadmap 中的能力已经全部落地。
> 表内 UmbraFlow 列:**✅=已交付并验证;◎=产品契约/目标(尚未落地或未验收)**。

| 维度 | 按键精灵/AHK | SikuliX | [ok-script](https://github.com/ok-oldking/ok-script) | **UmbraFlow** |
|---|---|---|---|---|
| 主要形态 | 桌面宏/通用自动化 | 屏幕视觉自动化 | 纯 Python 游戏自动化框架,覆盖 Windows/模拟器/浏览器 | C++ 宿主 + Luau 任务包,先做 Windows 原生游戏 |
| 后台运行 | 主要依赖前台/全局输入 | 主要依赖可见屏幕与前台交互 | ✅ 支持 WGC/BitBlt + PostMessage,也提供前台/全局输入后端 | ◎ **`background_only` 是 fail-closed 契约,不可静默降级** |
| 写任务 | 录制/专有脚本 | Python/Jython | 完整 Python + pip 生态,API 与业务代码同进程 | 受限 Luau capability API,资产与代码分离 |
| 视觉与适配 | 以坐标/找图为主 | 找图 | ✅ OCR、模板匹配、COCO 素材管理、自适应分辨率 | P0 最小模板识别;OCR/自适应按 P1 真实需求加入 |
| 脚本约束 | 通常拥有完整桌面能力 | 通常拥有完整进程/系统能力 | 完整 Python 能力面;灵活,但不是最小权限沙箱 | ◎ VM 配额、可硬停、确定性时钟/RNG、generation 热切换 |
| 失败证据 | 以日志/截图为主 | 以日志/截图为主 | 日志、截图/录制、静态截图回归与调试浮层;未把确定性回放声明为产品契约 | ◎ **observation/action 因果链 + 结构化 trace + 资源快照/回放** |
| 无人值守 | 能做,可靠性由脚本自担 | 能做,受桌面状态影响 | ✅ 已有多个真实游戏项目验证 | ◎ 核心目标,必须先通过长程稳定性验收 |

### 与 ok-script 的真正差异

ok-script 是 Python 自动化框架,不是一种脚本语言;它是 UmbraFlow 最接近、也最有价值的现实基准,不是用来陪衬的
弱对手。它已经提供 UI、截图、输入、OCR、模板匹配、调试浮层、静态截图回归、打包/升级、自适应分辨率与
多设备覆盖;在**能力广度、Python 生态、产品成熟度和现成游戏案例**上,当前明显领先 UmbraFlow。它也已经
证明 WGC/BitBlt + PostMessage 的后台技术路线可行。因此,"能后台"、"能找图"、"能写脚本"都不能单独成为
UmbraFlow 的差异化。

UmbraFlow 选择牺牲一部分 Python 生态与跨设备广度,换取三条更窄但更硬的产品契约:

1. **后台是不可降级的能力契约**:任务声明 `background_only` 后,前台激活、全局输入和移动真实光标从能力面上
   消失;目标不兼容就明确失败,而不是换一种会打扰用户的后端继续跑。ok-script 支持后台,但作为通用框架也
   暴露 PostMessage、前台 PostMessage、Pynput/PyDirect 等多种交互后端。
2. **动作必须能证明自己基于哪次观察**:`FrameId + TargetGeneration + ObservationLease` 把截图、识别和动作
   绑定成因果链;窗口重建、尺寸变化或旧帧都会让动作 fail-closed,避免"识别对了旧画面,却点到新状态"。
3. **无人值守建立在可复现证据上**:Luau 只获得最小 capability,宿主控制时钟、RNG、配额、取消和热加载
   generation;trace 固化脚本/资源/runtime 版本及 observation/action 结果,用于离线复核和确定性回归。

ok-script 也校准了 UmbraFlow 的另一个风险:**只有严格拒绝、没有适配,日常会很烦**。因此 P0 仍以
fail-closed 严格门防止误点,但从第一天保留统一坐标变换接缝;P1 优先补最小可用的均匀缩放自适应,
不让安全性变成每次分辨率或 DPI 变化都要重做脚本的维护负担。

所以唯一真正的护城河不是"能后台",而是 **把严格后台做成可验证协议,并为每次无人值守动作留下可复现证据**。
这也是 determinism/trace 必须留的原因:没有它们,"无人值守挂机"就是空话。

## 三、Roadmap(四阶段递进,每阶段"对我更有用一格")

> 锚:①最终形态 = 托盘常驻个人 App;②近期 = 单游戏做深。
> **标注工具是 P0 必需的任务制作入口**,不是后置美化;OCR/分辨率自适应仍在 P1 按真实日常需要拉入。
> P0 必须闭合完整作者路径:**截图 → 标注 UI/信息 → 生成资产与 manifest → 识别预览/截图回归 →
> 编写 Luau → 真机运行/trace**。
> **P0-A 必须交付一个类似 ok-script 标注体验的可视化系统**,不能用内部 API、CLI 裁图脚本或手改
> manifest 代替。相似的是“所见即所得地框选和验证”的作者体验;数据格式与安全契约仍采用 UmbraFlow 自己的设计。

### P0 — 能挂通(垂直切片)
- **目标**:证明作者能从真实截图制作可靠的页面识别资产,再由 Luau 驱动 observe/act 跑通卡厄斯梦境的
  **整套每日任务**(签到 → 打开面板 → 逐项前往 → 战斗 → 等结算 → 领奖 → 回主界面),严格后台、
  能 Ctrl-C 干净停。
- **P0 七块地基能力**:
  1. **等结算界面**:战斗结束判定 = `wait(结算模板, 分钟级 timeout)`(开发者确认走此最轻路径,不实时读战斗态)。
  2. **任意界面 → 主界面**:通用导航子流程 { 识别当前界面 → 点返回/关闭 → 重新观察 },直到主界面模板或超时。
  3. **长程运行**:单轮 `max_runtime` ~30 分钟(10–20 分钟实测 + 余量);战斗 `wait` 单步超时给分钟级。
  4. **截图采集与管理**:直接从 WGC 当前帧采集或导入 PNG;保存原始客户区尺寸、DPI、目标 generation、
     capture backend、时间与内容 hash,原图不被标注过程改写。
  5. **UI/信息可视化标注系统**:提供可独立启动的 GUI,在截图上缩放/平移、框选/调整/删除区域并命名;
     P0 至少支持
     `page_anchor`(页面识别锚点)、`action_target`(可交互目标)、`info_region`(文字/数字/图标/状态区域)三类。
     P0 统一使用有界灰度模板识别;只有真实每日必须读取动态语义文字/数字、且模板或状态锚点无法表达时,
     才另行裁决并提前拉入 OCR。
  6. **页面识别契约**:页面不是“看到一张小图就算命中”,而是一个具名 page signature;至少能表达
     required anchors 与 forbidden anchors。识别完成后,多页面命中返回 `Ambiguous`,全部不命中返回
     `Unknown`;两者都不能产生动作所需的 `ResolvedPage` 证据。取消、超时或预算耗尽是控制失败,
     不能伪装成 anchor 未命中。
  7. **资产生成与回归**:完整截图作为 authoring/测试源;标注分别定义模板裁剪 `template_rect` 与
     运行时搜索 `search_roi`,生成切分模板和 runtime manifest(稳定 ID、名字、类型、整数定点阈值、
     项目级 `base_resolution`/DPI、源截图 hash)。Preview 与 runtime 使用完全相同的有界识别策略,
     并把正例、负例和易混淆页面加入静态截图回归集与 Fake Controller 帧序列。
- **P0 标注系统的最小用户闭环**:
  1. 选择目标窗口并抓取当前 WGC 帧,或从磁盘导入一组截图;左侧截图/样本列表可切换当前画布。
  2. 画布支持缩放、平移、框选、移动、缩放、复制、删除及 undo/redo;坐标始终显示在明确的
     `base_resolution`/FrameSpace 中。
  3. 属性面板可编辑名称、标注类型、`template_rect`/`search_roi`、所属 page、整数定点阈值及
     required/forbidden 关系;
     `action_target` 可记录默认点击点,但不能绕过运行时 observation lease。
  4. 保存时完整 round-trip authoring document,并一键裁出模板、生成 runtime manifest 与 page signature;
     校验重名、越界、空区域和失效引用,作者不需要手改生成文件。
  5. Preview/Test 使用 runtime 同一有界识别策略,在当前图或全部样本上显示命中框、整数 SAD 边界、
     期望/实际 page、Unknown/Ambiguous 与停止原因;结果可直接加入正例、负例或易混淆回归集。
- **P0 标注系统的范围边界**:它是可日常使用的独立 authoring workbench,以后由 P2 App 打开,不是一次性
  debug 窗口。P0 不做任务图编辑器、脚本录制、素材市场、多人协作、完整项目管理或精美报告,但上述最小闭环
  任何一项都不能用“以后再补 UI”推迟。
- **⚠ P0 承重取舍(重要)**:"整套每日全跑完"意味着多个不同每日任务 + 大概率随机弹窗 + 长程中断点。
  **取舍:P0 允许"笨但能跑通",但弹窗不靠散落的 `if`**——因为战斗结束判定是分钟级阻塞 `wait(结算模板)`,
  阻塞期间散落 `if` 不跑,随机弹窗会漏 → fail-closed 停 → 整轮失败。故**最小 D6 拉进 P0**:每个观察
  周期边界 + 每个长 `wait` 内部做一次已知弹窗清扫(命中即关、继续);**重机制留 P1**(注册 API、
  first-match、max_hits、禁重入)。**D7 跨文件复用才是真能推的**:同文件 Luau 函数 + 复制粘贴让每日跑起来,
  代价是可维护性非可行性,P1 再重构。先拿到"每天真替我刷完"的价值。
- **交付顺序(A/B 穿插薄片,非线性;S0 锁死共享契约后 A/B 顺序不固定)**:真正共享的是
  **识别核(vision 模块已移植)+ authoring/runtime schema + page/action 证据 + 兼容指纹**;S0 一锁,
  A 的 Preview 直接用共享识别核、不必等 B 的引擎,B 消费 A 产出的资产。故拆成薄片穿插:
  - **S0 共享地基(已锁定,2026-07-23)**:识别核(已移植)+ authoring/runtime schema +
    `template_rect`/`search_roi` + page/动作证据 + 项目级尺寸/DPI 兼容契约。权威设计见
    [`2026-07-22-annotation-design.md`](2026-07-22-annotation-design.md);A1/B1 已解除设计阻塞。
  - **A1 最小标注**:抓 WGC 帧 / 导图 → 为一个 page anchor 与 action target 分别框选模板/搜索区域 →
    生成切分模板、最小单页 signature 与 runtime manifest。第一片即消除手裁 PNG;Preview 用共享有界识别核。
  - **B1 最小 runtime**:读 manifest → capture → 有界识别 → 唯一 page resolution →
    `ResolvedPage` + Detection 授权 → 严格后台点击 → trace。吃 A1 资产。
  - **A2**:扩展多 anchor/多 page 的 required/forbidden、Unknown/Ambiguous 与样本 Preview/Test。
  - **B2**:observe/act/wait 循环 + Tier A/B/C 错误 + 租约/generation + 逻辑时钟/RNG + 最小 D6 清扫。
  - **A3**:人体工学——undo/redo、样本列表、批量、静态回归集。
  - **B3**:整套每日(P0-C)+ 硬取消 ≤500ms + 遮挡/最小化/CaptureStalled + 长程。

  S0 之后 A/B 薄片只共享已锁定的 S0 契约 + 识别核,彼此不再硬阻塞,可自由穿插。
- **退出标准**:
  1. 开发者只通过可视化标注系统,不手改配置,即可从目标窗口截图或导入图片,完成框选、属性编辑、撤销/重做、
     保存和 Preview/Test;新 recognizer/page 立即能被 Luau 只读句柄引用。
  2. 卡厄斯梦境关键页面都有 page signature 及正例/负例/易混淆截图;固定回归集中预期页面全部命中,
     未知与歧义样本全部 fail-closed,修改模板/阈值后能立即重跑并看到差异。
  3. 整套每日无人值守跑完;全程后台不抢焦点;Ctrl-C 500ms 内干净停(含未释放输入补 Up);
     10–20 分钟一轮稳定完成;失败时 trace 能定位到截图、page/recognizer、confidence 与动作租约。
  4. 为当前游戏版本/分辨率留下 WGC + PostMessage 兼容性记录;遮挡场景必须通过,最小化若不产新帧则明确
     `CaptureStalled`/不支持,任何失败都不得降级到前台或全局输入。
- **依赖开发者输入**:~~第一条日常任务是什么、目标游戏~~ **已定:卡厄斯梦境 / 完成整套每日任务 / 等结算界面判定战斗**;
  仍需(可边跑边补,非阻塞):每日流程分解、关键页面截图/易混淆样本、运行分辨率。

### P1 — 好用到愿意每天开(单游戏做深 + 抽象回填)
- **目标**:把 P0"笨办法"跑通的整套每日**重构为干净抽象**;补齐日常真正卡脖子的能力。
- **交付**:用 **弹窗 interrupt**(D6)替换写死的弹窗 `if`;用 **跨文件子任务复用**(D7)消除多任务复制粘贴;
  按需拉入 **分辨率自适应**(D8)、**OCR**(若某日常需读数字);补标注工作台的批量样本管理、
  page confusion 诊断、资产重命名/引用更新与回归集维护体验。
- **退出标准**:开发者愿意每天用它替代手动;脚本干净可维护,不再是 P0 的"笨"形态。

### P2 — 常驻 App 成形(形态兑现)
- **目标**:CLI 升级为**托盘常驻 App**,形态兑现。
- **交付**:任务/项目列表点选启停、从项目入口打开标注工作台、实时调试浮层(识别框/状态,
  WDA_EXCLUDEFROMCAPTURE + WS_EX_NOACTIVATE)、trace 渲染成可点开的 HTML 报告、Windows 计划任务定时、
  设置/项目路径持久化与可直接启动的 portable/installer 构建。
- **退出标准**:开发者不再碰命令行,托盘里管理一切。
- **注**:这是"常驻 Engine"形态的正式落地;Engine API 函数边界在更早阶段就留好接缝(见 D10)。

### P3 — 铺第二个游戏(才谈通用性)
- **目标**:第二个游戏验证"核心零游戏分支"真成立,通用性从口号变事实。
- **放最后**:对个人而言一个游戏做深 > 两个游戏做浅。

### Roadmap 横向闭环检查

| 链路 | P0 | P1 | P2 |
|---|---|---|---|
| 任务制作 | 类 ok-script 的可视化框选标注系统、manifest/模板/page 生成、Luau | 批量样本、OCR/自适应、复用重构 | App 内项目/资产入口 |
| 页面识别 | page signature、Unknown/Ambiguous fail-closed | 弹窗 interrupt、confusion 诊断 | 实时识别浮层 |
| 验证 | 静态截图回归 + Fake Controller + 真机长程 | 每日回归集与兼容性扩充 | HTML trace 报告 |
| 运行 | CLI 单任务、严格后台、可靠取消 | 每天稳定使用 | 托盘、计划任务 |

原四个实现前设计点已由 S0 权威设计于 2026-07-23 锁定:

1. **标注/项目格式**:完整 GUI authoring document 确定性生成独立 runtime manifest 与切分模板。
2. **坐标语义**:一个项目级 `base_resolution`/整数 DPI 指纹;P0 使用 FrameSpace 整数像素和 identity gate;
   P1 另建显式 Base→Live viewport transform。
3. **page signature 语义**:required/forbidden 全局求唯一解;Unknown/Ambiguous 无动作能力;
   无优先级、阈值覆盖或启发式消歧。
4. **P0 authoring UI 技术栈**:Dear ImGui + D3D11,复用 WGC 与唯一有界灰度 SAD 内核;
   运行时浮层仍保持 `WS_EX_NOACTIVATE`/`WDA_EXCLUDEFROMCAPTURE` 纪律。

## 四、待定 / 待开发者输入

- **三个专属输入**(解锁 P0/P1 定形):~~第一条真实日常任务、目标游戏~~ **已定:目标游戏 = 卡厄斯梦境,
  首个日常 = 完成每日任务**(旧 DESIGN 里卡厄斯梦境本为最后的 M2 通用性验证游戏,现提为第一位)。
  仍待:每日任务的具体流程分解、脚本接管起点、运行分辨率、关键页面截图与易混淆/负例样本
  (Q8 提前自适应暗示需跨分辨率/DPI)。
- ~~**新版 DESIGN**:开发者手里有比旧版更新、含 ADR-013 的 DESIGN,需取得并 diff,复核 D0–D10~~
  —— **已解(2026-07-21)**:系误记,不存在第二版。`/e/github/UmbraFlow/DESIGN.md`(963 行,
  §24 含 ADR-013,AI 写的 v0.5 = commit bb267c3)即唯一版,与 vendored 副本逐字节一致;ADR-013
  与 D6 一致,无需 diff/复核。

## 五、脚本语言裁决:Luau

### 决策

- **P0 立即采用 Luau**;没有触发重评条件时,P1–P2 继续沿用。
- 研究基线为 Luau 0.730;接入时固定精确 tag/commit,升级必须经过中断、沙箱、确定性与热加载回归测试。
- C# 保留为未来**独立 worker 路线**候选,不作为当前默认。
- D0–D10 中与语言无关的任务语义、取消分层、trace 与 generation 思路继续作为存款;
  Lua/sol2 专属实现细节不构成约束,需映射到 Luau 后重新验证。

### 为什么当前选 Luau,不选 C# worker

| 判据 | Luau(进程内) | C#(独立 worker) | 当前裁决 |
|---|---|---|---|
| C++ 易集成 | 官方 CMake targets,直接嵌入 Compiler/VM | 需 .NET/Roslyn、进程生命周期、IPC 与协议版本 | **Luau** |
| 沙箱 | VM 原生 capability 白名单、readonly 环境 | 上限更高,但必须另做 OS 低权限与资源限制 | P0 **Luau 足够** |
| 死循环硬停 | interrupt safepoint + yield/abandon | 超时可杀整个 worker | C# 上限更高,Luau 满足 P0 |
| 确定性治理 | 小语言、单线程 VM、能力面窄 | 完整 BCL/线程池/反射扩大非确定性面 | **Luau** |
| 上手 | Lua 风格短脚本 + 可选类型/静态分析 | IDE/类型系统更强,但脚本与部署更重 | 当前偏 **Luau** |
| 热加载 | 新 VM generation 验证后原子切换 | 新 worker generation 原子切换 | 平手 |
| P0 成本 | 单体 C++ 垂直切片 | 提前增加第二工具链和分布式生命周期 | **Luau** |

当前产品是个人 App、单游戏做深,P0 首要任务是让第一条真实日常尽快形成闭环。脚本由开发者自己编写,
当前威胁模型主要是误写、死循环与资源失控,不是运行未知第三方恶意代码。Luau 已提供专门面向游戏脚本的
[沙箱原语](https://luau.org/sandbox/)与
[interrupt callback](https://luau.org/api/#callbacks);[官方测试](https://github.com/luau-lang/luau/blob/0.730/tests/Conformance.test.cpp#L3370-L3489)
覆盖 interrupt 中 yield 后由宿主放弃无限循环线程的模式。

C# 的强隔离收益主要来自**进程边界**,而不是语言本身。现代 .NET 不再把 CAS/AppDomain 当安全边界,
`Thread.Abort` 也不受支持;微软对不可协作终止代码的建议是放入独立进程后杀进程
([安全边界](https://learn.microsoft.com/en-us/dotnet/core/porting/net-framework-tech-unavailable)、
[线程终止](https://learn.microsoft.com/en-us/dotnet/standard/threading/destroying-threads))。
未来若必须升级到进程隔离,同一 worker 边界也可以继续运行 Luau,无需仅为获得 `Process.Kill` 而更换脚本语言。

### Luau 落地约束

- **每任务独立 VM generation**:独立 allocator 配额、全局环境与执行线程;任务结束即可整体回收。
- **只接收源码**:由受控 Luau compiler 生成 bytecode 并立即加载;不接受磁盘、网络或用户提供的 bytecode。
- **最小 capability API**:脚本只看到 `uf.*`;不暴露文件、网络、进程、环境变量、动态库与真实系统时钟;
  宿主 API 表及其嵌套对象递归 readonly。
  *(2026-07-29 修正:根由 `umbra` 改名为 `uf`,见
  [`2026-07-29-three-layer-task-system.md`](2026-07-29-three-layer-task-system.md) §6/§18。
  同文 §5/§7 进一步把 project 环境收窄为「`uf` 资源根 + `ctx`」,裸动词不在其中。)*
- **取消不可被脚本吞掉**:任务由 coroutine/`lua_resume` 驱动;其他线程只设置 atomic cancel;
  interrupt callback 检测取消后 yield,宿主不再 resume 旧线程。不得以可被 `pcall` 捕获的普通脚本错误作为最终取消信号。
- **宿主调用必须有界**:interrupt 只能抢占 Luau 执行,不能抢占卡死的 C++ binding。截图、识别、等待与输入 API
  必须支持 deadline/`stop_token`,不得无限同步阻塞。
- **确定性由宿主协议保证**:注入逻辑时钟和固定算法 RNG;禁止决策依赖 dictionary 遍历顺序;
  trace 记录 runtime/compiler 版本、脚本 hash、资源 hash、seed、observation、宿主 API 返回与 reload 事件。
- **热加载使用 generation swap**:后台编译并自检新 generation,只在任务安全点原子切换;
  不修补活跃调用栈、closure 或对象,持久状态只通过宿主定义的版本化 schema 迁移。
- **为未来 worker 留缝**:`IScriptRuntime` 边界只传可序列化 DTO;截图、识别、输入发送、按键持有账本和 trace
  归 C++ 宿主所有,禁止脚本持有 C++ 裸指针或不可序列化内部对象。
  *(2026-07-27 裁决:本条绑定的是未来跨进程 worker 接缝;P0 进程内脚本句柄用
  opaque userdata。**2026-07-29 修正**:原引用的 `docs/adr/0001-script-handles-are-userdata.md`
  已被开发者删除,该论证完整保留于
  [`2026-07-29-three-layer-task-system.md`](2026-07-29-three-layer-task-system.md)
  §11「为什么句柄不是可序列化 DTO」,四条理由与本条的绑定关系见该节;
  跨进程 worker 接缝拿到的是同文 §5 的 12 个原语签名。)*

### 一票否决验证

1. 普通与嵌套 `pcall` 包裹的无限循环均能在 P0 的 500ms 总退出目标内停止,且脚本不能恢复执行。
2. 无限分配、深递归与重字符串操作只终止对应任务,不得拖垮宿主进程
   (终止机制 = allocator 硬配额 + 指令/时间预算 interrupt;LUA_ERRMEM/栈溢出
   是可被 pcall 捕获后继续的普通错误,不构成本条的停机保证)。
3. 文件、网络、进程、环境变量、动态加载与真实时钟默认均不可访问。
4. 同一 observation trace + seed 连续执行 1000 次,action trace 与最终状态 hash 完全一致。
5. 新脚本编译或自检失败时旧 generation 不受影响;成功切换后不存在新旧 closure/对象混用
   (P0 验收口径 = 加载边界:编译+自检成功才原子安装,每 generation 全新 VM;
   运行中途活体热切推 P2 常驻 Engine——2026-07-27 裁决)。
6. 人为阻塞每个长耗时 C++ binding,验证其 cooperative cancel 能满足总退出预算。

### 重新评估 C# worker 的触发条件

任一条件成立即重开选型,而不是悄悄扩大 Luau 的安全承诺:

- 开始运行下载或共享来的未知第三方脚本;
- 要求即使 VM 漏洞或宿主 binding 卡死也绝不能影响主进程;
- 脚本演化为大型业务程序,明显需要 C# IDE、泛型、LINQ、`async/await` 或 .NET 库生态;
- 产品接受 .NET/Roslyn 发布体积、冷启动、双工具链、IPC 与 worker 生命周期成本;
- P0 一票否决验证中,Luau 无法满足沙箱、不可吞取消或 500ms 停止目标。
