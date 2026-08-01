# UmbraFlow C++ — 执行状态

> 顶层形态(三层 + Agent 操作者、两种信任模式、动词全集):[三层系统与 Agent 操作者](plans/2026-08-01-three-layers-and-agent-operator.md)。
> 层归属与第一层能力面(element/page 上移第二层、`cycle_*` 原语):[页面模型上移到脚本层](plans/2026-07-31-script-owned-page-model.md)。
> 标注语义(能力集合、holding、appearance、`cycle_read`):[标注模型重构](plans/2026-07-31-annotation-model-capabilities.md) §二 与 §四之二。
>
> **本文件只记执行状态。** 裁决与理由住在上面三份文档,失败知识住在 `docs/pitfalls/`;
> 这里只写「做到哪了」并给出指针,不复述机制。

## 已完成底座

一条一项,细节在指到的提交或 pitfall 里,不在这里。

- **Rust→C++ 移植**:domain / vision / controller / m0-demo。
- **WGC 真机截图**:卡厄思梦境 1600×900 客户区已验证。
- **提权 input-agent 后台点击验收**(2026-07-21 通过):严格后台 `PostMessage`、K2 delta=0、
  首次触发即租约 fail-closed。顺带发现的 WGC 静态页 stall 记在
  [`archive/plans/2026-07-20-post-port-win32-robustness.md`](archive/plans/2026-07-20-post-port-win32-robustness.md)。
- **B1 最小 runtime**(`modules/engine`):平台无关端口、runtime manifest 读取路径、versioned
  JSONL trace、Observation 句柄 API,fail-closed 全谱进 CI。`waitForPage` 已于 `8b16f2d` 删除,
  engine 不再轮询,改由可信 Luau framework 驱动 observe→resolve→find→act。
- **A1+B1 真机闭环**(2026-07-25 通过):标注后端生成 runtime manifest → release
  `umbra-flow run` → 真机后台点击,并串成两步导航链;全程不抢焦点、trace 干净。
  **真机一律用 release**:debug 识别 ≈1030ms 超 750ms 租约会 StaleObservation,release ≈32ms
  通过,不是产品 bug。根因、诱饵窗口过滤与多步编排陷阱见
  [`pitfalls/capture-and-target-selection.md`](pitfalls/capture-and-target-selection.md) 与
  [`pitfalls/page-modeling-and-multi-step.md`](pitfalls/page-modeling-and-multi-step.md)。
- **三层 task system 阶段 1–3**(权威见
  [`2026-07-29-three-layer-task-system.md`](plans/2026-07-29-three-layer-task-system.md) §17):
  地基、观察周期 + 两个环境 + `uf` 根 + Tier B userdata + 对抗套件、framework 承接 task policy。
  一票否决第 6 条的阻塞套件在 `1fb41a7` 进 CI,弹窗-长等待缺口在 `d1a0685` 关闭。
  该文的阶段 4「第一个真日常」即下面的 P0-C。
- **色键遮罩端到端**(`c392161`):vision 的带 mask SAD overload,`ColourKey` 烘进模板 PNG 的
  alpha 通道。阈值是掩码相对的,真正的驱动因素是掩码大小对 ROI 大小,见
  [`pitfalls/colour-key-annotation.md`](pitfalls/colour-key-annotation.md)。
- **`umbra-authoring` CLI**(`eacb05f`):第二个二进制,现在是唯一的标注工具,每一次改动都经过
  `AuthoringDocument`;同批加入 vision 的三个多帧分析原语(稳定性 / 色键探针 / 颜色普查)。
- **能力模型落地**:能力集合 `{identify, interact, read}`、引用侧 `Holding` 与 `exercised`、
  具名 `Appearance`(空列表 = 由页面定位)、`ElementId`、由引用派生的页面签名;词汇统一到
  element / appearance,三个 schema 升到 `umbraflow-authoring/v4`、`umbraflow-annotations/v3`、
  `umbraflow-trace/v2`,旧 id 没有读路径。证伪矩阵的 CLI 动词 `umbra-authoring check` 在
  `41e0816`,三态期望(`match` / `absent` / `unclaimed`)在 `d489979`。
- **workbench GUI 归档**(`b57b67b`):ImGui + D3D11 外壳、面板、文件对话框、一次性抓帧源、
  imgui submodule 与 ASan smoke fixture 一并移除;`entry/workbench` 只剩 `umbra-authoring`
  链接的标注后端。
- **`umbra-flow drive` 与 `key` 原语**(`ed38124`):第二个前端,是同一张私有能力面的**同级
  消费者**而不是通往 Luau 的口子;一个 generation 只上闩一个前端。`pressKey` 收
  `TargetGeneration`(按键不指名坐标),键名集合 52 个——2026-07-31 按真机界面加入 `ENTER` /
  `ESC` / `CAPS` / `SHIFT`。
- **OCR 接入**(`35c3447`):`IOcrEngine` 端口与 `TextLine` 词汇是平台无关的,PP-OCRv6_small +
  ONNX Runtime 适配器藏在 FFI 边界后,组合根是 `umbra-input-agent` 的 `read` 动词。
- **标注前端成为第三个 `trace::FrontEnd`**:`FrontEnd::Annotation` 已加,input agent 后端拆成
  `IInputAgentDrive` 与 `AnnotationSession` 两层;它自己的动词与事件仍未做。

## 当前工作:script-owned 迁移

权威是[页面模型上移到脚本层](plans/2026-07-31-script-owned-page-model.md)。四张工单:

- **工单 1 — 第一层原语面(§四)。已落地(2026-08-01,`1e71fb8` + `3118423`)。**
  `template_load` / `cycle_match` / `cycle_read` / `cycle_click_point`(裸点,仅第二层持有)/
  `project_read` / `project_write`(路径 confinement);`cycle_click` 接受本票据的 match 当
  受祝福证据;OCR 独立预算(每周期 8 次,耗尽是 `RecognitionIncomplete` 不是 miss);trace 走
  加法(`engine.text_read` + 可缺席字段)。CLI 经 `--ocr-models` 接入 OCR(run 与 drive 都有)。
  `cycle_page` / `cycle_find` 的退役归工单 4,原样活着。
- **工单 2 — 第二层 Luau 的 element / page / appearance 模型。已完成
  (2026-08-01,`bf471f3` + `88863cf`)**:`model` / `observe` / `project` 加页面图
  `navigation`(Edge/Graph/栈,深度护栏在触发前查,栈是信念观察是真相)与
  `observe.walk_edge`(interrupted 结局、逐页连击判定);工具函数抽进未发布的
  `mint`。页面图形状已由开发者裁决(script-owned §十.1 的 2026-08-01 注)。
- **工单 3 — 证伪矩阵迁到新基座(§七)。已完成(2026-08-01,`f630782`)。**
  `oracle`(屏与三态期望进 l2 文件)+ `regress`(层 2 判分:两两落空规则、分离系数)+
  `umbra-flow check`(文件帧源上的可信框架例程)。同帧页面证据回到**执法**:
  `resolve_page` 铸同票据回执,`observe.click` 与 `walk_edge` 都要;代价是 walk_edge
  会在触发帧重解析出发页——浮层压着时走不了被盖页的边,出路是先 pop 或按 §四之二.6
  拆页(声明处有记)。跨边界成本实测 ~723µs/格,其中 ~650µs 是每调用固定开销
  (trace 两行 + 逐行 flush + 指纹检查),批量原语按 §十.5 只记不做。
  遗留文档账:CONTEXT.md 还不认识 oracle/regress/回执,工单 4 的文档批一起补。
- **工单 4a — 旧动词退役。已完成(2026-08-01,`73c7c6f`)。**
  `cycle_page` / `cycle_find` / `wait_for_page` / `CapabilitySurface`(含 `uf.elements`、
  `uf.pages`)/ engine 的 `resolvePage`、`findAction`、`act` 与 catalog 点击路径全部退役;
  `cycle_click` 只吃 match。模板胶水迁 `vision/template-match`、`ProjectFingerprint` 迁
  `domain/space`、`FrameIdentity` 迁 `domain/frame`,**`engine -> annotation` 边已切断**
  (engine manifest = core domain ocr trace vision),指纹改由 l2 文件提供。预 VM 校验器
  改看 `page-model.toml`(`task/page-model-file`,C++ 只扁平扫名)。drive 收缩为七个裸动词。
  九处突变九红。**遗留**:trace 的 `Page::Score` / `elementId` 仍用 annotation 类型,
  产品已无发射方,随 trace v3 修剪。
- **工单 4b — Agent 前端与探索环境。实现已落地(2026-08-01,`9b5c8bc`),
  真机验收已通过。** 形状见
  [Agent 前端与探索环境](plans/2026-08-01-agent-front-end-and-exploration.md)。
  新原语 `cycle_crop`(带 sha256,自有每周期预算,不消费周期)与 `probe`(色键统计,
  无键时选择字段缺席而非补零);`ScriptTrustMode` 分环境,`explore` / `scribe` 只在
  探索环境,裸点击不再对**任何**项目环境可命名(顺手堵了 `ctx:cycle_click_point` 曾
  公开给所有项目环境的洞);`umbra-flow explore` 队列通道走 `FrontEnd::Annotation`,
  其流**结构性拒绝** `engine.action_delivered`。14 突变 14 红。
  **真机验收(2026-08-01)**:探索通道 crop → census(5259 色,主色 250,245,254)→
  白键探针(29140 像素中 8901 全选)→ OCR「進入」92.8% → `scribe` 写入新元素
  `sortie_enter_button`(interact+read,阈值 9000)→ 重载确认 → 手工补引用与四条期望 →
  `umbra-flow check` 100 格 `accepted=true` 零发现。
  **仍缺**:`scribe` 不会写页面引用与边(本次手工补),补齐后才做删除波。
- **工单 4c — §九 删除波。未开始。** 前置:`scribe` 的引用/边动词。删 v4 标注生产线
  (`entry/authoring` 绘制动词与 v4 `check`、`entry/workbench` 标注后端、
  `modules/annotation` 模型层与 recognition 栈、两个旧 schema 读写路径、
  `entry/input-agent` 与 `entry/m0-demo`),trace v3 修剪,依赖图定格,文档批
  (CONTEXT 的 oracle/regress/回执欠账一并清)。

- **第一条真边已走通(2026-08-01,真机)**:`walk-first-edge` 任务全程走新栈——
  l2 文件 → 图 → 栈 → 等 home → walk_edge → 回执授权点击 (1438,240) → sortie
  连击确认,exit 0;trace 干净(1 次授权 1 次投递 1 次作废,33 次 cycle_match)。
  证据:`E:\umbraflow-projects\chaos-v14\walk-trace.jsonl` 与落地帧。旧引擎动词
  (cycle_page/cycle_find)全程未参与——script-owned 的运行路径自此是活的。

迁移配套(2026-08-01 夜):

- **chaos-v14 已按 v4 重放重建**(`E:\umbraflow-projects\chaos-v14`,3 页 22 元素,全命中,
  `check` 92 格零发现;翻译脚本 `session-0731/author-*-v4.ps1`)。两个真实行为变化记录在案:
  interact/read 的模板走 `element appearance`(`page add` 只为 identify 铸 appearance);
  deploy_danger 合并单元素后 read 路径改用实测的 9900 bp 阈值,比旧 info 元素严。
- **chaos-v14 的 l2 页面模型已落成(2026-08-01)**:`page-model.toml`(l2-v1,22 元素 /
  3 页 / 2 边 / 4 屏 / 70 期望),期望从 v4 实测格转录并加严,`umbra-flow check`
  首跑 `accepted=true` 零发现。已知缺口在该文件的产出报告与 §gaps:sortie_alt 屏
  整体 unclaimed(上游无 regression 行)、回边(back/sortie_home)未断言待走边验证、
  regress 暂不吃 reference 的 rect_override(汉堡以 v4 `match --page` 补验过)。
- **真机只读验证(release + chaos-v14 + `--ocr-models`)已两层全通(2026-08-01)**:
  机制层——绑窗、抓帧、live 帧 `cycle_read`、`engine.text_read` 证据、置信度守门
  (2.7–4.1ms/次);内容层——菜单唤醒后 12 个连续周期页面 Resolved,「故事」矩形
  连读 6 次 `conf=10000bp` 全对。待机 CG 与点击唤醒的抓帧相关性记入
  [`pitfalls/capture-and-target-selection.md`](pitfalls/capture-and-target-selection.md)。
  顺带:出擊那格读出低置信错字——read 矩形要框纯文字,不要连图标,归 Agent 手册。
- **两条小裁决已定(2026-08-01,开发者授权自行决定)**:deploy_danger 合并后的 9900 bp
  阈值**认下**(量出来的数,行为变化有意为之);onnxruntime.dll 部署步**并入工单 3**。
- **release bin 缺 onnxruntime.dll 随附——已还清(`1df9ef0`)**:根因是共享 bin/ 只被
  input-agent 的拷贝步顺带喂饱,孤立构建目标时回落 PATH 加载旧 1.17.1 崩溃;
  `cpp_stage_runtime_libraries` 把拷贝挂到每个可达 ocr 的目标自己身上。

长期挂着的阻塞(不属于任何一张工单):

- **真机验收需要开发者**——实现方够不到目标机器,凡带「真机」字样的验收都要排队等人。
- **OCR 整块模式未接线**——`TextLayout::Block` 目前直接返回 `UnsupportedCapability`,只有
  `SingleLine` 可用(det 模型已在 `modules/ocr/external/models/`,适配器还没跑它)。
- **CI 计费挂起**——2026-07-24 起每个 job 即刻失败,门禁只能本地跑;账单恢复后 `gh run rerun`。

## P0-C — 卡厄斯梦境完整每日

- [ ] 分解签到、每日面板、逐项前往、战斗、等结算、领奖、回主界面的真实流程与接管起点。
- [ ] 用 P0-A 制作全部页面、控件和信息区域资产;文字读取确实阻塞时才提前引入 OCR。
- [ ] 用 Luau 写完整每日;P0 用**最小 D6 弹窗清扫**(观察周期边界 + 每个长 `wait` 内)+ 同文件复制,
      不建 D6 重机制 / D7 跨文件复用(留 P1)。
- [ ] 全程后台、不抢焦点;Unknown/Ambiguous/StaleObservation 均 fail-closed 并留下可诊断 trace。
- [ ] 整套每日连续稳定跑完一轮,Ctrl-C 500ms 内退出,单轮 10–20 分钟。

## P1–P3 后续

- [ ] P1:弹窗 interrupt、跨文件子任务、均匀缩放自适应、按需 OCR、标注批量维护与 confusion 诊断。
- [ ] P2:托盘 App、项目/任务入口、运行时浮层、HTML trace 报告、计划任务、portable/installer。
- [ ] P3:第二个游戏验证核心零游戏分支。

## 延迟的健壮性台账

- [ ] 在 P0-C 前补遮挡、最小化/CaptureStalled、投递中 Ctrl-C 与 10–20 分钟长程验证。
- [`archive/plans/2026-07-20-post-port-win32-robustness.md`](archive/plans/2026-07-20-post-port-win32-robustness.md)
  —— HWND 复用竞态、best-effort Up、DPI 与 capture 取消边界。
- [`plans/2026-07-20-m0-demo-port-deviations.md`](plans/2026-07-20-m0-demo-port-deviations.md)
  —— 移植期有意偏差与后续清理项。
