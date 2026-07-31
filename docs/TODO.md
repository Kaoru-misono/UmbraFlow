# UmbraFlow C++ — 当前执行清单

> 状态基线:2026-07-23。产品方向与阶段退出标准以
> [`2026-07-21-product-form-and-roadmap.md`](plans/2026-07-21-product-form-and-roadmap.md) 为唯一权威;
> Luau 任务语义以
> [`2026-07-21-lua-task-model-grill-decisions.md`](plans/2026-07-21-lua-task-model-grill-decisions.md) 为实现层存款。
> S0 标注共享契约以
> [`2026-07-22-annotation-design.md`](plans/2026-07-22-annotation-design.md) 为权威。
> 本文件只记录执行顺序,不重复维护产品裁决。
>
> **执行按 A/B 穿插薄片**(S0 共享地基 → A1/B1/A2/B2/A3/B3,顺序不固定;详见 Roadmap 交付顺序):
> 下面 §1/§2/§3 按 P0-A/B/C 归类**能力**,不代表严格先后。**S0 已于 2026-07-23
> 获开发者批准并锁定**,A1/B1 不再受设计阻塞。

## 0. 现有底座与真机收尾

- [x] Rust→C++ 移植:domain / vision / controller / m0-demo。
- [x] WGC 真机截图:卡厄斯梦境 1600×900 客户区已验证。
- [x] 完成提权 input-agent 的后台点击 before/after 验收(2026-07-21 通过):头像切换 ×3、
      标签切换 ×3、模态识别+安全关闭;严格后台 PostMessage 投递、K2 delta=0;真机首次触发
      租约 fail-closed(StaleObservation)。发现 WGC 静态页 stall,记入
      [`2026-07-20-post-port-win32-robustness.md`](plans/2026-07-20-post-port-win32-robustness.md)。
- [ ] 在 P0-C 前补遮挡、最小化/CaptureStalled、投递中 Ctrl-C 与 10–20 分钟长程验证。

## 1. P0-A — 可视化标注系统

> **进展(2026-07-30)——两项能力落地,下面的 checkbox 尚未按它们重述:**
>
> - **色键遮罩,端到端**(`c392161`)。游戏把右侧菜单的白字直接画在会变的立绘上:同一处 UI
>   在两种背景下逐像素灰度平均差 56.8 / 26.8,而字形像素本身逐字节相同。作者取色,颜色决定
>   拿哪些像素来比,同一对目标降到 0.28 / 0.18。落地形状:vision 的 `matchTemplateSad` 增加
>   两个带 mask 的 overload(增量,现有调用一处未改;全不透明 mask 与无 mask 逐位一致),
>   annotation 把 `ColourKey` 烘进模板 PNG 的 **alpha 通道**(无新资产、无 manifest 变更;
>   不带键的模板字节不动),workbench 加取色器与选中像素叠加预览。**注意小掩码陷阱**:
>   带色键的模板实测需要严得多的阈值(真机 9000 bp → 9900 bp),细节见
>   [`pitfalls/colour-key-annotation.md`](pitfalls/colour-key-annotation.md)。
>
>   > 更正(2026-07-31):此处原本写「阈值按模板全部像素数推出,**对 mask 一无所知**」。
>   > 那个机制说法是错的——`normalizedScore` 会把加权和缩回整模板尺度,所以阈值其实是
>   > **掩码相对**的。真正的驱动因素是**掩码大小对 ROI 大小**:27 个饱和白像素总能在大 ROI
>   > 里找到一个全对齐的位置。该 pitfall 已于同日按此更正,这里跟着改,免得两处机制打架。
> - **`umbra-authoring`**(`eacb05f`)——第二个二进制,开发工具,让 agent 不开 GUI 也能标注
>   一个项目。**(2026-07-31 起它是唯一的标注工具:GUI 已归档。)**它自己不写任何东西,每一次改动都经过 `AuthoringDocument`,因为标注产物就是点击
>   授权的证据。同批加入 vision 的三个帧分析原语(稳定性 / 色键探针 / 颜色普查),它们**收 N 帧
>   而不是两帧**:两种背景找出「不随背景变的 UI」,几秒后再拍的第三帧才找出「正在动的东西」。
>   `match` 子命令是重点:标注、对着留出来的截图验证、迭代,回路里没有人。
>
> 顺带修掉两处会直接卡死高 DPI 标注的(同在 `eacb05f`):`importSourcePng` 曾把每张导入的 PNG
> 盖成 96 dpi,而 `AuthoringDocument` 拒绝指纹与项目不符的 source,于是 1600x900@144 的项目
> 一张截图也吃不进;CLI 又从这个约束反向推错,把自己的项目钉死在 96 且不给 `--dpi`。
> 文件没有显示密度,唯一可能正确的是窗口的密度,也就是项目的密度——这不是便利,而是因为
> 运行时在 live 指纹与 catalog 不符时拒绝投递点击,所以按错密度标注的项目是不可用而不是近似。

> **进展(2026-07-31)——标注模型换代 + GUI 弃用,下面 §1 的 checkbox 尚未按此重述。**
> 裁决与理由见
> [`2026-07-31-annotation-model-capabilities.md`](plans/2026-07-31-annotation-model-capabilities.md);
> 该文顶部的「落地进度」块是逐条状态。要点:
>
> - **能力模型已落地。** 三选一的 `AnnotationType` 变成能力集合
>   `{identify, interact, read}`;`bool shared` 换成引用侧的 `Holding{Owned, Referenced}`;
>   `allowed_page_ids` 整个删除,授权就是「这一页引用了它且行使 `interact`」;元素带的是
>   有序的具名 `Variant` 列表(空列表 = 由页面定位);`RecognizerId` 改名 `ElementId`;
>   页面签名不再是作者写的,而是从引用派生。两个 schema 一次原子升到
>   `umbraflow-authoring/v3` 与 `umbraflow-annotations/v2`,旧 id 没有读路径。
> - **GUI 已归档**(`b57b67b`):`entry/workbench/app/`、ImGui + D3D11 外壳、文件对话框、
>   一次性抓帧源、imgui submodule 与 ASan smoke fixture 一并移除;没有 `umbra-workbench.exe`,
>   没有 `--smoke`,没有 `AsanSmoke` 测试标签。`entry/workbench` 剩下的是
>   `umbra-authoring` 链接的标注后端(编辑层、`preview.*` 的证伪矩阵、持久化、源图导入)。
> - **CLI 动词收敛**:`page add-anchor|add-target|add-info` → `page add --capability C...`;
>   `--shared` 退掉;新增 `page reference`(把已有元素放到第二个页面);`match` 增加 `--page`。
> - **仍欠的**:证伪矩阵还没有 CLI 动词、还没有 variant 维度(§2.3 的 P1–P4 / R1–R4 一条未落);
>   CLI 还没有画 variant 的动词;`read` 能力的 `cycle_read` 与 OCR 接线未开始。

- [x] 锁定 authoring/runtime schema、`template_rect`/`search_roi`、page resolution、动作证据、
      项目级尺寸/DPI 兼容契约与 Dear ImGui + D3D11 技术栈(2026-07-23)。
      **注(2026-07-31)**:schema 与技术栈两项都已被推翻——schema 见上面的进展块,
      Dear ImGui + D3D11 随 GUI 一起归档。其余四项仍然有效。
- [ ] ~~独立 GUI~~(**2026-07-31 弃用**,保留原文作为它曾经存在过的记录):
      WGC 抓帧/导入图片、样本列表、画布缩放/平移、框选编辑、undo/redo。
      **A1 最小实现已落地(2026-07-24)**:`umbra-workbench`(Dear ImGui 1.92.8-docking +
      D3D11)、四面板、`--smoke` 自检通过。**2026-07-25 真机 GUI 使能修复**(`fix(workbench)` 提交):
      WGC 抓帧在高 DPI 目标上采用线程级 per-monitor 感知 + 真实 DPI 串入 source fingerprint(否则
      1066×600 虚拟几何 / 96 DPI manifest → umbra-flow 指纹不符);ImGui 载入中文系统字体(否则中文标题显示 `？`);
      启用 docking(vendored 已是 docking 分支,原先没接线)。
      **2026-07-25 人工 GUI 走查暴露并修复的可用性/正确性缺口**(见
      [`pitfalls/workbench-authoring-ui.md`](pitfalls/workbench-authoring-ui.md)):
      ① recognizer 只在新建那一刻可选中(`setSelectedRecognizerId` 全项目仅一处调用),
      建了第二个后第一个永久不可达 → 新增 **Recognizers 面板**,选中时同步跟到它所属 source
      (否则框会画在无关图上);② page 只在选中某 recognizer 时才在属性面板露一眼且**无法删除**
      → 新增 **Pages 面板**;③ **完全没有删除**(建错只能立刻 undo)→ 补
      `deleteRecognizer`/`deletePage`/`deleteSource`,连带撤出 page signature、清授权、
      删源上的回归用例,级联会触及只有作者能决定的东西时改为拒绝并给出可执行提示;
      ④ `page_anchor` ↔ `action_target` 类型切换在逐控件提交模型下**无解**(两条规则互锁)
      → `retypeRecognizer` 单事务改写类型及全部依赖字段;⑤ 默认名固定导致第二次新建必撞
      (名字跨 recognizer/page 全局唯一)→ 取第一个空闲 `<stem>_N`;
      ⑥ 成功编辑不写日志,只有失败留痕 → 每次被接受的编辑都要带描述;
      ⑦ **既有 use-after-free**:属性面板持有 `RecognizerDefinition const*` 贯穿整帧,
      而每次提交 `m_current = std::move(next)` 会整体换掉 document → 改为
      `PendingEdit` 延迟提交,在借用 document 的面板画完之后、actions 面板之前统一 apply。
- [ ] 标注能力:`identify` / `interact` / `read`(**2026-07-31 起是集合,不再是三选一的
      `page_anchor` / `action_target` / `info_region`**);编辑 `template_rect` 与
      `search_roi`,以及页面引用、整数定点阈值和 required/forbidden 关系
      (required/forbidden 现在挂在**引用侧**的 `exercised.identify` 上,不在元素上)。
      **A1 属性面板已覆盖全部字段(2026-07-24)**——该面板已随 GUI 归档,现在的作者入口是
      `umbra-authoring page add --capability`;多 page 编辑体验留 A2。
- [ ] 保存可完整 round-trip 的 authoring document,一键生成切分模板与 runtime manifest,无需手改配置。
  - [x] 平台无关后端:canonical authoring document 严格往返、完整引用校验、源 PNG hash/尺寸校验、
        内容寻址模板与 runtime manifest 的纯确定性编译(PNG 编码配置已 pin,2026-07-24)。
  - [x] Workbench 文件保存、内容寻址资产发布、以 runtime manifest 为提交点的完整生成集
        顺序发布(2026-07-23)。**注**:发布非跨 artifact 原子且无回滚——manifest 发布失败会留下
        新 authoring 文档 + 旧 runtime 闭包(见 `project-persistence.hpp` 注释),P1 决定是否加固。
  - [x] Workbench 读取路径 `loadAuthoringProject`:annotations.toml + 源 PNG hash/几何校验的
        完整重开(2026-07-24)。
- [ ] 使用 runtime 同一有界灰度 SAD 策略 Preview/Test,显示命中框、整数边界、
      Unknown/Ambiguous 与停止原因。
- [ ] 建立卡厄斯梦境关键页面的正例、负例和易混淆静态截图回归集。

## 1.5 B1 — 最小 runtime(modules/engine,2026-07-24 落地)

- [x] `modules/engine`:平台无关端口(IFrameSource/IActionSink 带租约透传/ITraceSink)、
      runtime manifest 读取路径、versioned JSONL trace、Observation 句柄 API
      (observe→session.resolvePage/findAction→act,动作即失效;取消与目标失活在投递边拦截)。
- [x] `umbra-flow run`:第一个同时链接 engine+controller 的组合根;
      发现→指纹→会话→waitForPage→findAction→act,区分退出码,Ctrl-C 进 stop_token。
      **注(2026-07-29,`8b16f2d`)**:这条流水线记的是 B1 当时的形状。`waitForPage` 已随
      `PageWait` / `sweepKnownPopups` 一起删除,engine 现在完全不轮询;CLI 也不再调用任何
      engine 动词,而是把端口交给 `task::TaskHost`,由受信任 Luau framework 的
      `ctx:wait_for_page` 驱动 observe→resolve→find→act。见
      [`plans/2026-07-29-three-layer-task-system.md`](plans/2026-07-29-three-layer-task-system.md) §16。
- [x] Fake IFrameSource 合成帧回放 + fail-closed 全谱(Unknown/Ambiguous/stop reasons/
      租约过期/指纹不符/失效句柄复用 → 全部零投递)进 CI。
- [x] **真机冒烟(2026-07-25 通过,release + 生产 750ms 租约)**:手写最小 manifest,
      卡厄思梦境识别+授权+后台点击。**release `umbra-flow run` 在无任何租约放宽下**:识别精确
      (page anchor + action target,sadScore=0)、授权、后台 `PostMessage` 投递、页面真的从
      「能力值」切到「卡牌」、trace 全程干净(ClickDelivered,无 StaleObservation)、
      全程游戏不获前台焦点、K2 delta=0。fail-closed 亦已验证(debug/过期租约 → StaleObservation 零投递)。
      **debug↔release 说明**:先前 debug 构建识别 ≈1030ms 超 750ms 租约导致 StaleObservation;
      release 识别 ≈32ms 通过——故真机/生产一律用 **release**,无产品 bug、无需改代码
      (「2fps 渲染」结论已作废)。根因与实测见
      [`pitfalls/capture-and-target-selection.md`](pitfalls/capture-and-target-selection.md)。
      顺带修复:反自动化诱饵窗口(数十个不可见同名窗口)——`selectCandidate` 加可见性/非最小化过滤;
      WGC 绑定需与目标同完整性级别(提权)。(CJK `--selector` 经核实无 bug:已嵌 UTF-8 manifest,
      当时 mojibake 是测试输入构造失误。)
- [x] **真机端到端 A1+B1 闭环(2026-07-25,数据路径已验证)**:workbench 授权后端(抓帧/ingest/
      `buildAuthoringDocument`/`saveAndGenerateAuthoringProject`)生成完整 runtime 项目 →
      release `umbra-flow run` 吃该 workbench 生成的 manifest → 真机后台点击成功
      (能力值→卡牌,sadScore=0,ClickDelivered,不抢焦点)。经临时程序化 driver 驱动整条后端链路验证,
      无需人工 GUI 操作即证明 authoring→生成→runtime→真机点击闭环。
- [x] **纯 GUI 人工走查 + 多页面导航链(2026-07-25 通过)**:开发者全程鼠标操作,在 workbench 里
      抓两屏(主界面 / 角色详情)、标 2 个 `page_anchor` + 2 个 `action_target`、建 2 个 page、
      保存生成 → **两次 release `umbra-flow run` 串成一条导航链**:主界面点 `battleCharacter`
      → 进角色详情 → 点 `meiling` → 进角色特写,两步均 `exit=0`。
      trace:step 1 先 11 帧 `PageUnknown`(画面未稳)才 `PageResolved`,
      `sadScore=9780 / maximumSad=299880` → `ActionAuthorized` → `ClickDelivered(1469,558)`
      → `ObservationInvalidated`;step 2 **首帧**即 `PageResolved(page_1)`、`sadScore=0`
      → `ClickDelivered(513,287)`。**step 2 能解析出 `page_1` 本身就是 step 1 点击生效的证明**
      ——若未跳转,step 2 会 poll 到超时并 fail-closed,而不是盲点。
      离线复刻匹配器(同灰度、同阈值公式)验证两个 anchor 交叉方向余量 2.85–4.15 倍,
      故**不需要 forbidden 互斥**(此前凭「两个模板都在左上角」的猜测被实测推翻)。
      注意 `sadScore` 一个 0 一个 9780:实机帧与标定静态图必然有漂移,
      9000 bp 阈值留了约 30 倍余量才是它能过的原因,不是「截图与实机一模一样」。
      多步编排现状与页面建模陷阱记入
      [`pitfalls/page-modeling-and-multi-step.md`](pitfalls/page-modeling-and-multi-step.md)。

## 2. P0-B — Luau Engine

> **2026-07-27**:脚本层动工前 grill 已完成,A 类裁决(`umbra.*` 命名空间、
> `modules/task` 绑定模块、userdata 句柄、任务归属项目、热加载加载边界口径等)
> 与执行切片见 [`2026-07-27-p0b-script-layer.md`](plans/2026-07-27-p0b-script-layer.md)。
>
> **2026-07-29**:脚本层现行权威改为
> [`2026-07-29-three-layer-task-system.md`](plans/2026-07-29-three-layer-task-system.md)
> (三层 = C++ 保证层 / 可信 Luau framework / project task)。它取代
> `2026-07-28-luau-first-task-system-design-draft.md` 全文,以及上条 2026-07-27
> 裁决中的脚本层部分。**上条的 `umbra.*` 命名空间裁决已被撤销:根改名为 `uf`**
> (§6/§18);句柄/任务寻址口径不变但改由该文 §11/§6 承载。落地顺序见其 §17,
> 明确删除清单见其 §16。
>
> **进度(2026-07-29,核对至 `1fb41a7`)**:该文 §17 的**阶段 1(地基)、阶段 2
> (观察周期 + 两个环境 + `uf` 根 + Tier B userdata + 对抗套件)与阶段 3
> (framework 承接 task policy)均已完成**。下面这几条 checkbox 尚未按它重述,读时以
> 该文 §16/§17 的状态标记为准。
>
> 上一版这里写「仍未落地的是一票否决第 6 条本体」——**已作废**:它于 `1fb41a7` 落地进
> CI,是 `tests/task/test-veto-blocking.cpp`(八个能阻塞的原语逐个阻塞,四个豁免写明
> 理由,见该文 §8)。阶段 3 的出口两条也都已核对:弹窗-长等待缺口在 3b `d1a0685` 关闭,
> `ctest -L CI` 在 `1fb41a7` 的干净工作树上 16/16 通过。
>
> 余下的是**阶段 4(第一个真日常 + 真机验收全项)**——由开发者裁决何时开始——
> 以及条件性的阶段 5(跨文件复用,仅当阶段 4 证明需要)。
>
> **进展(2026-07-30,`ed38124`)——阶段 4 开工前先长出了第二个前端与一个新原语:**
>
> - **`umbra-flow drive`**:命令以 JSON 行追加进 `--queue`,每条命令一行结果 flush 进
>   `--results`。它是同一张私有能力面的**同级消费者**,不是通往 Luau 的口子——保证层
>   (周期账本、四要件点击授权、指纹检查、trace)本来就在 Luau 之下的 C++ 里,`ctx.luau`
>   只是它的一个消费者。**一个 generation 只接受一个前端**,`startTask` 与
>   `startOperatorSession` 中先到的那个上闩,另一个报 `UnsupportedCapability`。
>   便利命令(`wait_page` / `find_click`)**不带任何自己的默认值**,每个 timeout 与轮询间隔
>   都是必填字段,于是 `ctx.luau` 仍是 task 侧 policy 的唯一住处。
> - **`key` 原语**(私有表 12 → 13 个)。目标游戏是键盘驱动的:`E` 结束回合、`A`/`S` 开牌堆、
>   `F1`-`F3` 换角色、数字键选手牌;而点卡牌不会选中它(真机验证),出牌需要拖拽,当时没有
>   任何前端能做。`IActionSink::pressKey` 收 `TargetGeneration` 而 `click` 收
>   `ObservationLease`——按键不指名坐标,没有会过时的矩形。它要求周期打开并花掉周期,
>   为此 `CycleLedger` 多了 `spend` 与 `consume` 并列。键名集合是 `domain::KeyName` 这一份
>   (A-Z / 0-9 / F1-F12,48 个,无别名)。
>   **2026-07-31 更正并加宽**:上一条里「出牌需要拖拽」是错的——战斗界面自己印着
>   `ENTER 使用卡牌` / `ESC 取消選擇` / `CAPS 確認卡牌資訊`,出牌就是 ENTER;真机复核也确认
>   选中卡牌后点击目标敌人是**取消选择**而不是出牌,所以鼠标不是替代路径。键名集合因此加入
>   具名键族 `ENTER` / `ESC` / `CAPS` / `SHIFT`(同屏右上角的锁定开关),变成 52 个名字对应
>   52 个虚拟键;`TAB` 不收,因为没有观察到任何界面提供它。集合仍然封闭、仍然区分大小写,
>   拒绝信息改为把整套词汇和大小写规则都写出来。
> - trace 每条事件多一个 `frontEnd`(`"task"` / `"operator"`),校验状态机在 operator 流上
>   拒绝 `framework.*`。
>
> 真机验证过(实现方够不到真机):`cycle_open` 绑定窗口、`cycle_find` 把一次判定为未命中的
> 搜索报成 ok 而不是错误、按键投递并记为 `engine.key_delivered`、同一个已花掉周期上的第二次
> 按键被 `StaleObservation` 拒绝、每条事件都带 `frontEnd=operator`。m0-demo 路径上两次按键
> 把手牌从 06/10 变成 05/10、弃牌堆变成 6+1、第一个敌人从 352 掉到 239。
>
> 这几条不改变阶段 4 的定义,但改变谁能驱动它:第一个真日常既可以是 task,也可以先由操作者
> 逐步驱动出来。下面的 checkbox 尚未按此重述。

- [ ] 固定 Luau 精确版本,接入 compiler/VM 与 `IScriptRuntime` 可序列化边界
      (边界口径见
      [`2026-07-29-three-layer-task-system.md`](plans/2026-07-29-three-layer-task-system.md)
      §11「为什么句柄不是可序列化 DTO」:P0 进程内 userdata,DTO 归未来跨进程
      worker 接缝——原 `docs/adr/0001` 已删除,论证迁至该节)。
      **进展**:0.730 submodule + RAII `Engine::runNumber` 已落地(2026-07-22);
      沙箱、记账 allocator 配额与 interrupt 硬取消也都已落地
      (2026-07-29 核对:`installSandbox` / `createStateWithQuota` /
      `ffi/cancellation.cpp`),此前这里写的「沙箱/取消/配额未做」已过时。
      历史记录见 `2026-07-21-p0b-luau-hardening-ledger.md`。
- [ ] 最小 capability API 与 observe/resolve/act/wait 引擎循环;manifest 只读 recognizer/page 句柄,
      `ResolvedPage` + Detection + lease 才能授权坐标动作。
- [ ] 每任务 VM generation、allocator 配额、interrupt 硬取消、逻辑时钟/RNG 与 generation 热加载
      (热加载 P0 只做加载边界语义,活体中途热切推 P2——2026-07-27 裁决)。
- [ ] Fake Controller 帧序列、结构化 trace、资源快照和静态截图回归接入 CI。
- [ ] 能力/兼容性门、项目级尺寸/DPI 指纹、P0 identity Base→Live gate、持续重校验、
      租约校验与动作后强制作废观察。
- [ ] 通过 Roadmap 第五节的 6 条 Luau 一票否决验证。

## 3. P0-C — 卡厄斯梦境完整每日

- [ ] 分解签到、每日面板、逐项前往、战斗、等结算、领奖、回主界面的真实流程与接管起点。
- [ ] 用 P0-A 制作全部页面、控件和信息区域资产;文字读取确实阻塞时才提前引入 OCR。
- [ ] 用 Luau 写完整每日;P0 用**最小 D6 弹窗清扫**(观察周期边界 + 每个长 `wait` 内)+ 同文件复制,
      不建 D6 重机制 / D7 跨文件复用(留 P1)。
- [ ] 全程后台、不抢焦点;Unknown/Ambiguous/StaleObservation 均 fail-closed 并留下可诊断 trace。
- [ ] 整套每日连续稳定跑完一轮,Ctrl-C 500ms 内退出,单轮 10–20 分钟。

## 4. P1–P3 后续

- [ ] P1:弹窗 interrupt、跨文件子任务、均匀缩放自适应、按需 OCR、标注批量维护与 confusion 诊断。
- [ ] P2:托盘 App、项目/任务入口、运行时浮层、HTML trace 报告、计划任务、portable/installer。
- [ ] P3:第二个游戏验证核心零游戏分支。

## 延迟的健壮性台账

- [`2026-07-20-post-port-win32-robustness.md`](plans/2026-07-20-post-port-win32-robustness.md)
  —— HWND 复用竞态、best-effort Up、DPI 与 capture 取消边界。
- [`2026-07-20-m0-demo-port-deviations.md`](plans/2026-07-20-m0-demo-port-deviations.md)
  —— 移植期有意偏差与后续清理项。
