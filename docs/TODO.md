# UmbraFlow C++ — 执行状态

> 顶层形态(三层 + Agent 操作者、两种信任模式、动词全集):[三层系统与 Agent 操作者](plans/2026-08-01-three-layers-and-agent-operator.md)。
> 层归属与第一层能力面(element/page 上移第二层、`cycle_*` 原语):[页面模型上移到脚本层](plans/2026-07-31-script-owned-page-model.md)。
> 标注语义(能力集合、holding、appearance、`cycle_read`):[标注模型重构](plans/2026-07-31-annotation-model-capabilities.md) §二 与 §四之二。
>
> **本文件只记执行状态。** 裁决与理由住在上面三份文档,失败知识住在 `docs/pitfalls/`;
> 这里只写「做到哪了」并给出指针,不复述机制。

## 已完成底座

一条一项,细节在指到的提交或 pitfall 里,不在这里。

> 本节记的是**当时发生过什么**。其中 `umbra-authoring`、`umbra-input-agent`、
> `m0-demo`、workbench 标注后端与 `modules/annotation` 都已于 2026-08-01 随工单 4c
> 删除(`a80ea07`),量出来的事实仍然有效,二进制不必再去找。

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
  该文的阶段 4「第一个真日常」即下面的 P0-C——那一格现在是**一局出擊**,不是日常。
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
  ONNX Runtime 适配器藏在 FFI 边界后;组合根现在是 `umbra-flow` 的 `--ocr-models`
  绑定(`entry/cli/platform/ocr-engine-binding.*`)。
- **标注前端成为第三个 `trace::FrontEnd`**:`FrontEnd::Annotation` 已加,input agent 后端拆成
  `IInputAgentDrive` 与 `AnnotationSession` 两层;它自己的动词与事件仍未做。

## 当前工作:script-owned 迁移

权威是[页面模型上移到脚本层](plans/2026-07-31-script-owned-page-model.md)。四张工单:

- **工单 1 — 第一层原语面(§四)。已落地(2026-08-01,`1e71fb8` + `3118423`)。**
  `template_load` / `cycle_match` / `cycle_read` / `cycle_click_point`(裸点,仅第二层持有)/
  `project_read` / `project_write`(路径 confinement);`cycle_click` 接受本票据的 match 当
  受祝福证据;OCR 独立预算(耗尽是 `RecognitionIncomplete` 不是 miss;上限 2026-08-01 随
  `cycle_read_lines` 从 8 提到 32,理由写在 `k_defaultMaximumReadsPerCycle` 上);trace 走
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
  - [ ] **遗留文档账,已复核过一半(2026-08-03)**:CONTEXT.md 现在认识 `oracle` 与
        `regress`(196 / 230 / 299 行),但**一处都没提回执**(`receipt`,grep 零命中)
        ——而同帧页面证据正是靠它执法,`observe.click` 与 `walk_edge` 都要。
- **工单 4a — 旧动词退役。已完成(2026-08-01,`73c7c6f`)。**
  `cycle_page` / `cycle_find` / `wait_for_page` / `CapabilitySurface`(含 `uf.elements`、
  `uf.pages`)/ engine 的 `resolvePage`、`findAction`、`act` 与 catalog 点击路径全部退役;
  `cycle_click` 只吃 match。模板胶水迁 `vision/template-match`、`ProjectFingerprint` 迁
  `domain/space`、`FrameIdentity` 迁 `domain/frame`,**`engine -> annotation` 边已切断**
  (engine manifest = core domain ocr trace vision),指纹改由 l2 文件提供。预 VM 校验器
  改看 `page-model.toml`(`task/page-model-file`,C++ 只扁平扫名)。drive 收缩为七个裸动词。
  九处突变九红。~~**遗留**:trace 的 `Page::Score` / `elementId` 仍用 annotation 类型~~
  ——已随工单 4c 的 trace v3 修剪掉(2026-08-03 复核:`modules/trace/source/` 里两个名字
  都已不存在)。
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
  **`scribe` 的引用与边动词已补齐**:`add_reference` / `add_edge` 重建冻结的页面与图
  并按身份重映射每条指向旧页面的边(该性质只能靠身份断言证明——保存出的字节两种写法
  一样);12 突变 12 红。
  **§三 第二条验收线已通过(2026-08-01,真机)**:Agent 经探索通道写出
  `sortie --click(sortie_enter_button)--> deploy` 这条边并落盘,`walk-agent-edge`
  任务走它,授权点击 (801,817),画面从出擊页跳到戰鬥員配置页。整条链——量像素、定键、
  写元素、写边、走边——全部由 Agent 自己完成。
- **工单 4c — §九 删除波。已完成(2026-08-01,`a80ea07`)。** 删除 151 文件 / 54462 行:
  `modules/annotation` 整体、`entry/authoring`、`entry/workbench`、`entry/input-agent`、
  `entry/m0-demo` 及其测试;trace 升 v3(去掉已无发射方的页面分数与 `elementId`)。
  两处搬迁:`ContentHash` 去 **domain** 而非 core(它返回 `Result`,而 core 没有错误
  词汇);`ElementId` / `PageId` 直接死掉——trace v3 删掉了它们唯一的消费者。
  OCR 模型 staging 跟着 `umbra-flow` 走,`--ocr-models` 在构建树里照常可用。
  **依赖图已定格成 script-owned §三 的形状**,10 个模块无环,只剩一个二进制
  `umbra-flow`(run / drive / explore / check),CI 目标 18 → 13。
  **删除波暴露的滚轮缺口已补**(`c62d730`):`cycle_scroll` 在 run 与探索两个面上都有,
  不收坐标、不查指纹、引擎侧不查租约,要开着的周期并花掉它,投递边复验目标实例;
  15 突变 15 红,其中两条是**加法式**突变——证明「不做坐标那套围栏」是守住的而非漏写。
  投递瞄准客户区中心,因为随便命名一个锚点等于顺手回答了 2026-08-01 文档 §九-5,
  那条仍开放。已知不对称:controller 自己会查租约年龄,所以旧观察上的滚轮会在
  投递边被拒(引擎不拒)。
  - [ ] **抓帧唤醒那条经验规则仍未移植进探索通道。**
  **滚轮的另一半在 2026-08-03 补齐**:`ctx:cycle_move_pointer(ticket, x, y)` 从
  `controller::movePointer` 一路打通到脚本面(`IActionSink::movePointer` /
  `EngineSession::movePointer` / `TaskContext::cycleMovePointer`)。它指名坐标,
  所以引擎侧拿的是 clickPoint 的**整套**围栏(取消、句柄、指纹、租约、实例复验、
  花掉观察),一条不减;而它按不下任何东西,所以按 `cycle_scroll` 的方式两个面都
  发、`ctx` 直接转发——裸坐标的特权护的是「激活页面没授权的东西」,移动激活不了。
  trace 加 `engine.pointer_move_delivered`(加法,schema 仍 v3),点走 `clickClient`。
  14 突变 14 红。
  - [ ] **`umbra-flow drive` 的操作者协议既没有 scroll 也没有 move 动词。**两个动词在
        脚本面都有了,只有操作者这一侧缺——所以人手复现一次脚本的动作序列做不到,
        而那正是诊断「脚本这一步为什么没落地」的手段。

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
  连读 6 次 `conf=10000bp` 全对。待机 CG 与点击唤醒的抓帧相关性属于目标自身的
  行为,记在工程目录下的 `PITFALLS.md`(2026-08-01 从主仓库迁出)。
  顺带:出擊那格读出低置信错字——read 矩形要框纯文字,不要连图标,归 Agent 手册。
- **两条小裁决已定(2026-08-01,开发者授权自行决定)**:deploy_danger 合并后的 9900 bp
  阈值**认下**(量出来的数,行为变化有意为之);onnxruntime.dll 部署步**并入工单 3**。
- **release bin 缺 onnxruntime.dll 随附——已还清(`1df9ef0`)**:根因是共享 bin/ 只被
  input-agent 的拷贝步顺带喂饱,孤立构建目标时回落 PATH 加载旧 1.17.1 崩溃;
  `cpp_stage_runtime_libraries` 把拷贝挂到每个可达 ocr 的目标自己身上。

长期挂着的阻塞(不属于任何一张工单):

- **真机验收需要开发者**——实现方够不到目标机器,凡带「真机」字样的验收都要排队等人。
- ~~**OCR 整块模式未接线**~~——已接线(2026-08-01)。适配器跑 det 模型,`TextLayout::Block`
  返回区域里每一行文字与它自己的矩形(目标像素,不是裁剪相对);原语是
  `cycle_read_lines`,第二层动词是 `observe.read_lines`。预算按「定位算一次、每找到一行
  再算一次」从同一个读预算里扣,超限是响亮的 `RecognitionIncomplete` 而不是截断的列表。
  (2026-08-03 追加:两个读动词各多一个无默认值的空区域参数,空读算「这里没有」还是
  「我没读到」由调用方声明;见 `docs/plans/2026-08-01-three-layers-and-agent-operator.md`
  的「读区域」条。)
  det 模型和 rec 模型一起随 `umbra-flow` 部署,`--ocr-models` 少一半就在启动时点名拒绝。
- **CI 计费挂起**——2026-07-24 起每个 job 即刻失败,门禁只能本地跑;账单恢复后 `gh run rerun`。
- ~~**探索会话的 Luau 堆不跨 chunk 回收,裁几十张整帧就撑爆**~~——已修(2026-08-02),
  但**当初记的根因是错的**。症状照录(2026-08-01 真机实测):循环里反复
  `explore.crop(0,0,1600,900)`——每张约 1.8MB 的 PNG 字符串——之后**任何**分配都失败,
  连单次 200×200 的 `crop` 加 `probe` 都报 `not enough memory`;重开会话拿到干净 VM
  立刻恢复。错误句子只说「not enough memory」,不说是谁吃掉的,定位花了三个 chunk。
  真实根因不是「不跨 chunk 回收」,而是**压力下根本不回收**:记账 allocator 在
  `memoryQuotaBytes`(默认 64 MiB)处拒绝增长并返回 null,而 Luau 拿到 null 就地
  `luaD_throw(L, LUA_ERRMEM)`(`VM/src/lmem.cpp:248/505/545`)——它**没有 PUC Lua 那套
  emergency GC 再重试**。于是那个上限量的是「存活 + 尚未被收掉的垃圾」。这也是为什么
  只在 chunk 边界补 GC 治不好它:撞上的那个循环在**一个 chunk 内部**。
  落地三处:`Engine::collectGarbage()` / `heapUsage()` 与 `lua_State` 版自由函数;
  `ExplorationSession::evaluate` 在扫 cycle 之后整收一次;`cycle_crop` 落字符串前,
  剩余空间不足待分配字节的四倍时先整收一次。另外每行结果带
  `"heap":{"used":…,"ceiling":…}`,失败时若剩余 ≤ 上限的 1/8,把两个数字缀在 Luau
  原句之后。规则与两个证伪陷阱见
  [`pitfalls/embedded-vm-memory-ceiling.md`](pitfalls/embedded-vm-memory-ceiling.md)。
- ~~**色键蒙版没有接进标注通道**~~——已接上(2026-08-02)。`explore.crop` 现在收一个
  可选色键(位置与默认容差和 `probe` 完全一致),把权重烘进裁剪 PNG 的 alpha 通道,
  `decodeTemplateImage` 读回来就是匹配器的 mask 平面;不带键时字节与从前一致。
  第三个返回值是**刚画出来的那张蒙版**:`rect_pixels` / `selected_pixels` /
  `ramp_selected_pixels`,外加一句在计数落到两端之外时才出现的 `warning`。
  `scribe.measure` 把同一个键同时喂给裁剪和探针(从前只喂探针,于是量的是一件事、
  存的是另一件),`scribe.author_element` 把宿主**实际用的**那个键写进 appearance,
  文件里是 `key = [r, g, b, tolerance]`——蒙版在 alpha 里,键在文件里,一年后还能重画。
  两条标注失败的处置**不同**,理由见
  [`pitfalls/colour-key-annotation.md`](pitfalls/colour-key-annotation.md):
  **一个像素都选不中的键当场拒绝**(它烘出来的是全透明模板,任何一次匹配都会在
  `sad.cpp` 里 abort,那不是质量问题而是根本不可用),**太小/太满的蒙版只警告不拒绝**
  (计数只是从形状上猜,68% 的橙色填充照样过关,真正的判据是证伪矩阵)。
  **原来的代价是可测的**:目标的小地图把节点画成深色网格上的小亮块,84×58 的框里图标只占
  一小部分,于是分数被背景主导——同一张模板对「亮节点/暗节点/空格子」打出
  8885 / 8549 / 8582,三者分不开;而右侧那面又大又饱和、几乎填满裁剪框的旗子,
  战斗 9297–9998 对未知 8451–8557,中间有八百基点的空档。
  也就是说**能不能用模板,取决于图标占裁剪框的比例**,而色键正是把这个比例从
  「图标 vs 整框」变成「图标 vs 图标」的那一步。**小地图那条路的机制阻塞已解除**,
  真机重标仍未做。
- **多 appearance 的折叠搜索在大搜索区域上买不起**(2026-08-01 标注一局出擊时撞上)。
  `find` 对一个元素的每个 appearance 各搜一遍,代价是 **appearance 数 × 搜索区域
  面积**,两个因子都在标注期定死、在调用处看不见。实测:两个 appearance 铺在
  300×850 上,连**十秒**的动作帧租约都跑不完。当天的出路是**拆元素**——一个类型
  一个元素、各自一个小矩形——立刻就通了,说明主导项是面积不是个数。
  代价是这条出路和模型裁决**相反**:同一矩形上互斥的状态本该是**一个**元素带命名
  appearance 列表(见[标注模型重构](plans/2026-07-31-annotation-model-capabilities.md)
  §四之二.4(a) 与[三层系统与 Agent 操作者](plans/2026-08-01-three-layers-and-agent-operator.md) §六)。
  裁决没有错,它只是**只在小矩形上付得起**;大搜索区域上今天没有第二条路。规则那一半记在
  [`pitfalls/element-choice-and-thresholds.md`](pitfalls/element-choice-and-thresholds.md)。
  - [ ] **标注期把这个代价说给作者听。**今天没有任何东西这么做:`appearance 数 × 面积`
        两个因子都在标注期定死,而付账在运行期,作者看不见自己刚签的字。最便宜的形状是
        `scribe.add_appearance` / `author_element` 在算得出面积时给一句警告——和色键
        「太小/太满的蒙版只警告不拒绝」同一条路子,因为真正的判据仍是证伪矩阵。

标注通道的三处缺口已补(2026-08-01):

- **文字元素现在能被声明了**——`scribe.claim_text(built, screen, element, state, text)`
  和 `scribe.claim` 并列,分法与 `author_text_element`/`author_element` 同一条:
  一格只由一种证据量,选了哪种要写在那一行里。判定仍全在 `oracle.Expectation.new`,
  scribe 只过字段。纯探索通道现在能把一个工程从零标到底。
- **`check` 的每周期读预算改成从被检的文件里读**——`k_defaultMaximumReadsPerCycle`
  的 8 是给「读一次轮询一次」的 wait 循环设的上限;矩阵不是循环,它每屏开一个观察、
  每个元素最多读一次,所以需要多少是文件的属性。`check` 现在按工程声明的元素数取
  上限(`TaskHost::projectElementCount`),并把这个数一起格式化进例程——例程用
  `oracle.Claims.most_reads_on_one_screen` 在开第一屏之前比一次,不够就点名两个数字
  拒绝。超限依旧是响亮的控制错误,永远不会变成静默的 miss。
  换屏重开周期这条路走不通:帧来自按文件名顺序一次一张的目录,重开就是下一屏的像素。
- **空工程的骨架和那句读不懂的报错**——`umbra-flow explore` 开工前铺
  `assets/templates/`、`assets/screens/`、`frames/`
  (`entry/cli/project-skeleton.*`;模型层不建目录,`ProjectFileStore` 也不能建——
  「写入没跑出工程外」正是靠 canonicalize 一个真实存在的父目录证明的)。
  拒绝本身也重写了:点名缺的是哪个目录、用调用方写的工程相对拼法。
  `script error: (non-string error value)` 的根因是 `lua_tostring` 不走元方法,
  已由 `script::RaisedError` 把宿主自己那句话带到边界上。

矩形的来源变成三选一(2026-08-02):

> 一个矩形可以由**元素**、由**页面引用**(`rect_override`)、或由**期望**(`rect`)提供;
> 任何一次使用恰好有一个提供者。

起因是标注小地图。图标要在**脚本每帧算出来的**格子上匹配——地图会平移,格子位置是这一
帧的事实,不是模型能知道的东西。而 `Element.new` 从前强制 `rect`,于是那四个元素只能带
着「模板从哪儿切的」那个框,模型把它当事实陈述出来,而它是假的;更糟的是它们不被任何
页面引用,矩阵**根本没法给它们打分**——一条无法被证伪的标注。

`rect` 因此改成可选。五条拒绝保证「可选」不会滑成「没人说去哪儿找」:

- 引用一个不画矩形的元素、又不给 `rect_override`
- **用不画矩形的元素做 identify**——identify 扫描发生在页面确定之前,那时没有「这一页
  的行」可查,它只能搜元素自己的矩形,所以这种元素不可能参与页面签名
- 期望指向不画矩形的元素、又不带矩形(「没有地方可看,所以没有任何东西能反驳它」)
- 期望给一个自己画矩形的元素**另**指一个矩形,那会让声明偷偷挪动它被评判的那次测量
- 拿裸元素去读一个不画矩形的元素

期望带矩形还买到一件更大的事:**同一元素可以在同一张截图上被声明多次,各带各的矩形**。
今天手跑的那张小地图分离表(命中 9989–9999、最好假阳性 8488)因此能写成常驻回归,而不是
活在一句注释里的一次性数字。这正是当天连犯两次的那个错的解药——阈值只拿一个样本定死,
9500 挡住了 9465/9395 的真阳性。

`reading.confusions` 的键从 `(元素, 文字)` 改成 `(元素, 矩形, 文字)`。这是**主动放宽一
条守卫**:一个元素被放到九个地方之后,九个「確認」都读到「確認」,原规则会报成「一个区
域分不开九张截图」——一条天天误报的规则,作者迟早连同它真正要防的案子一起关掉。对每个
自己画矩形的元素,键完全没变。

`scribe.author_unplaced_element` 是配套的第三个动词。现有两个按**验证来源**分(像素 /
读数),它按**放置方式**分。没有它,手改文件能表达的形状 agent 通道造不出来——正是这个
仓库命名过的「一扇门守住了,形状跟另一扇一样的洞还开着」。传 `rect` 会被点名拒绝,而且
拒绝发生在写模板之前,被拒的那一行不留资产。

`scribe.add_appearance` 补的是同一扇门上的另一个洞。一个元素在两天里遇到四张不同的节点
徽记,每次都只能手改 TOML:`add_element` 拒绝已存在的名字,而元素是冻结的,追不上去。
新动词把元素重建一遍,再把每个引用它的页面、每条指向那些页面或点击它的边一起重建。后半
步才是要害——引用行持有的是元素对象而不是名字,漏掉重建的页面照样存出正确的文件,只是
一直搜昨天的 appearance 列表,下游没有任何东西看得见。`add_reference` 现在也按身份拒绝
已被取代的元素。它是一个动词而不是 `author_` / `add_` 一对:appearance 没有铸造方,拆成
两半只会造出一张 agent 自己也能写的普通表。12 突变 12 红。

顺带纠正一处构建约束。`embed_luau.py` 按「相邻字面量拼接也算进 MSVC 的 65535 上限」这条
理由,拒绝任何超过 60000 字节的 `.luau`,而 `scribe.luau` 一路在往这条线上撞(55159 字节
是写这一条时手上的量值;`cef4886` 落地后它已是 70652 字节,也就是说上限真正删除时,这个
文件早已越线一万多字节)。实测(MSVC 14.44):单条 70000 字节的字面量确实报 C2026,而 500000 字节拆成 250
条相邻的 2000 字节字面量在 `/W4` 下干净通过、拼接长度的 `static_assert` 成立。所以那条按
文件的上限守的是一条不存在的限制,真正的守卫一直是 `MAXIMUM_CHUNK_BYTES`;上限已删除,
测量写进了注释。

一处诚实的降级:`check.cpp` 的 `readBudgetForCheck = 2 × 元素数` 从**精确上界**变成启发
式,因为无矩形元素能在一屏上被声明多次。主要那一半仍在开跑前被 `Claims.most_reads_on_one_screen`
大声拦住;剩下的会在遍历中途变成 `RecognitionIncomplete` 这个响亮的控制错误,不会变成一
格被静默报成 miss。公式没动——改它属于 CLI 的设计决定。

## P0-C — 卡厄思梦境一局出擊

这一节原本写的是签到 / 每日面板 / 逐项前往 / 领奖那条日常,**没有人在做那条流程**,
整节按实际在建的东西重写:**一局出擊**——Agent 经探索通道逐页标注,第三层
dispatcher 一次认一页、做那一页要做的一件事,把这一局走完。

工程在 `E:\umbraflow-projects\chaos-daily`,目标自身的行为记在它自己的 `PITFALLS.md`。
可复用的标注手艺进主仓库:
[`pitfalls/element-choice-and-thresholds.md`](pitfalls/element-choice-and-thresholds.md)
(标什么、阈值怎么定)与
[`pitfalls/page-modeling-and-multi-step.md`](pitfalls/page-modeling-and-multi-step.md)
(浮层与候选页排序、动作是否落地、护栏)。

- [x] **逐页标注已完成(2026-08-01,Agent 自己做完)**:`page-model.toml`(l2-v1)
      27 页 / 102 元素 / 17 appearance / 122 引用 / 10 边 / 29 屏 / 81 条期望,
      20 张模板。102 个元素只有 17 个 appearance——**识别以读文字为主,模板只留给
      没字的图标**,这是这一轮最大的形状变化。
- [x] **第三层 dispatcher 在跑**:`explore-0801-daily/verbs/`。`step.luau` 是
      「认出一页、做那一页要做的一件事」,`whereami.luau` 是丢线之后的全量回退;
      候选页的排序纪律(浮层先问、共享锚点最后问)写在 `step.luau` 的 `ORDER` 上,
      理由在 pitfall 里。
- [x] **量不出区分度的元素已经换掉或改法**:角落交叉双剑(真假阳性只差 88 基点)
      换成填满裁剪框的徽记,10000 命中;極限开关的阈值改由真机连采定(存帧 9993,
      真机 9401);手牌区改成整块读,位置从帧里来,不从矩形里来。
- [x] **整局连续跑通**(2026-08-03 真机达成)。`umbra-flow run --task daily`,一个进程、
      无人值守,从第 1 步 `sortie -> 進入` 到第 85 步「一局是一次任务;再按出擊会开第二局」
      自行停机;13 场战斗、7 次分支选择,`runOutcome=Completed`,单轮 **12 分 10 秒**
      (729578 ms)。trace:`frames/menu-to-menu5.jsonl`(21154 行)。
      **周期账目是平的**:3992 次 `cycle_open` = 3790 次 `cycle_close` + 202 次被投递花掉
      (63 裸点 + 1 点击 + 138 按键),一个都没漏——而今晚死过两次的恰恰是周期泄漏。
      授权 64 次、投递 64 次,1:1。12405 次宿主调用**零拒绝**(10761 Succeeded /
      1644 Empty)。全程 3 次抛错都在**框架层**:`cycle_read_lines` 读回 Empty,调用方声明
      的是 `empty_is_unknown`,于是抛 `recognition_incomplete` 交给脚本重试——空区域策略
      按设计生效,宿主一次都没拒绝。
- [ ] **小地图那条路还没重标**:节点图标分不开(亮/暗/空 8885 / 8549 / 8582)。
      挡路的机制已于 2026-08-02 解除——`explore.crop` 收色键、把蒙版烘进模板 alpha,
      见上面那条已划掉的阻塞;剩下的是拿一个真机会话按节点自己的亮色重标一遍。
- [ ] **不抢焦点:量到一次,但那次的证明力不够**(2026-08-03)。做法是一边让探索会话投递
      `cycle_move_pointer` 与一次空白处 `explore.click_point`,一边每 10ms 采样一次
      `GetForegroundWindow`:146 次采样**没有一次**焦点落到目标窗口上,投递本身
      `ok=true`。**但当时机器是锁屏的**,前台一直是锁屏界面——这个读数分不清「从不抢焦点」
      和「当时根本抢不到」。要作数得在解锁、且前台是另一个普通窗口时重跑同一个探针。
      探针在 `scratchpad/focus-probe.py`。
      顺带量到一条以前不知道的事:**锁屏状态下 WGC 抓帧照常拿到真内容**(400×200 的裁片
      78170 字节,不是黑帧),因为它抓的是窗口自己的合成而不是桌面。也就是说存档屏之外的
      离线活,不必等机器解锁。
- [ ] **Unknown / StaleObservation 的 fail-closed 没有被验到**:那一局零拒绝,
      所以这两条路一次都没走过——要验得专门造一次失效观察。
- [x] **Ctrl-C 500ms 内退出:已验(2026-08-03,真机 release)**。5 次测量
      **16 / 32 / 32 / 47 / 31 ms**,均值 32 ms,十倍余量。做法是一个只花抓帧、
      不投递任何输入的诊断任务 `chaos-daily/tasks/idle-cycles.luau`,配
      `scratchpad/interrupt-probe.py` 用 `CTRL_BREAK_EVENT` 打断并计时——目标一个像素
      都没动过。trace 两头完整,332–334 行。
- [ ] **单轮时长已实测(12 分 10 秒),「稳定」还没有**:目前只有一次成功,
      连跑多局的重复性没量过。

## Luau 代码规范(2026-08-02 测量完,一行未改)

仓库有 C++ 规范,**没有 Luau 规范**;`modules/task/runtime/*.luau` 那 15 个文件全靠习惯
维持写法。已按六个维度量过一遍,提纲、故意不裁决的六件事、以及按价值排序的 15 项改动
都在 [`plans/2026-08-02-luau-coding-standard.md`](plans/2026-08-02-luau-coding-standard.md)。
下面三条是量出来的**缺陷**,不是风格问题,单独列出来等批准:

- [x] **`observe.luau` 的错误层级**(2026-08-03 修复)。`requireCtx` 原本抛 level 2,却被
  这个文件里 7 个公开 verb 调用,于是报错指向 framework 自己的源码而不是工程脚本里
  真正传错的那行;同文件的 `readTarget` 用的是 3,注释写的正是前者违反的规则。现在
  `requireCtx` 抛 3(一个助手的栈是 调用者 → 公开 verb → 这里)。
- [x] **error level 现在有测试**(2026-08-03)。`tests/task/test-framework-surface.cpp`
  「a misused verb names the script's line, not the framework's」:四个公开 verb 各用坏
  ctx 调一次,断言报错位置指向 chunk 自己而不是做检查的 framework 模块。证伪:把
  `requireCtx` 改回 level 2、重编、立刻红(返回 -1,即第一个 verb 的位置已经不指向
  调用方),改回 3 立刻绿,`observe.luau` 按字节还原(md5 复核)。
- [x] **整套不可变约定从未被证伪**(2026-08-03 补上)。
  `tests/task/test-script-owned-model.cpp` 新增用例 "Every value the framework hands
  a script refuses a write":28 行数据,每行取 framework 交给工程脚本的一个值,先写一个
  新键、再改一个已有的键,两次都必须抛 `attempt to modify a readonly table`。断言的是
  **写被拒**,不是 `isfrozen` 那个标志位——契约是拿到的值改不动,只有写证得了。覆盖
  Element / Page / Reference / Hit / Receipt / Edge / Graph / Claims,以及它们里面嵌的
  每一层:引用行与引用表、外观行与外观表、能力集、矩形、色键、边的去向集、图的四张表、
  Claims 的五张索引表。证伪:`model` / `navigation` / `oracle` / `evidence` 四个文件里
  28 处 `table.freeze` 逐个删掉、逐个重编译,28 次全部只让对应那一行转红,没有一次仍然
  全绿,也没有一次波及别的行。
- [x] **`mint.frozen_extra` 是浅冻**(2026-08-03 两半都修了)。`frozen_extra` 现在
  逐层复制并冻结,深度上限 8 层(与 `mint.derives_from` 同一个理由和同一个数):自引用
  的表由 `check_extra` 出句子、构造器在 level 2 抛,指向作者自己那行。`project.encode`
  的 `renderValue` 收一个 `field` 参数,遇到「键不是 1..n 的表」直接拒绝并点名那个键——
  **选拒绝不选写 inline table**,因为写 inline table 等于给 `l2-v1` 加一种值类型,
  旧构建读到就整份文件报错;而且 `parseValue` 不认 `{`,得连解析器一起改。
  证伪(改前实测 score = 15/15):`element.extra.tags == mine.tags` 为真、改调用方的表
  元素跟着变、直接写 `element.extra.limits.retries` 成功、`project.encode` 写出
  `limits = []`。改后三个用例全绿,把 framework 那两个文件换回改前版本立刻全红。

规范的主干不是命名而是**线格式**:数据字段 `snake_case`、Luau 变量绑定永不 `snake_case`
(672 处字段 0 处驼峰),因为字段名同时是 TOML 键、脚本 API 和类型字段,拼错的后果是
`project.parse` 把它归进 `residual`、`project.build` 从不读它——文件看着没问题,
字段没了。这一行相对 C++ 规范是**反的**。

另外要先承认:今天没有任何机制检查 `.luau`——`fix_format.py` 的扩展名列表里没有它,
也没有任何类型检查跑过,所以 8907 行里每一个 `--!strict` 目前都只是带高亮的注释。

> **2026-08-03 补上一半。** `.luau` 已进 `fix_format.py`(386→401 个文件),而且和 C++
> 共用「缩进必须是空格」那条,所以制表符也会被拦——扫过一遍,现有 15 个文件本来就干净,
> 加进去零改动。证伪:往 `docs/` 放一个带制表符的 `.luau`,`--check` 立刻转红。

**2026-08-03 量完了。** 用树里那份 Luau(0.730)的 `CLI/src/Analyze.cpp` 构建出
`luau-analyze`,对 `modules/task/runtime/*.luau` 跑一遍(`--solver=old`,即 `--!strict`
今天实际对应的那个求解器;默认的新求解器噪音大得多,171 条):

- **145 条诊断,其中 141 条是同一件事**——`Unknown global 'model' / 'mint' / 'oracle' …`。
  这不是缺陷,是 bundle 的形状:15 个模块之间靠**全局**互相可见而不是 `require`,
  名字由 C++ 的 `frameworkProjectGlobals()` 发布。检查器不知道这件事,所以每一处跨模块
  调用都报一次。
- **真正的类型错误只有 4 条**,其中 3 条已修(见下),剩 1 条是检查器自己的毛病。

已修的三条(2026-08-03,`project.luau`):`table.insert(document.blocks, rawTarget)` 传的是
声明为 `{ string }?` 的局部,现在先绑一个必然非空的 `block`;两处
`for … in appearancesFor[name] or {} do` 迭代的是联合类型 `{any} | {}`,现在先落到一个
具名的 `{ any }` 局部上。三处都是**读起来也更清楚**的改法,不是为了让检查器闭嘴。

剩下那一条不修:`regress.luau` 的 `local ok, walkError = pcall(walkScreen, …)`。`walkScreen`
不返回值,而 Luau 的 `pcall` 类型签名里**根本没有错误通道**(`(f: (A…) -> R…) -> (boolean, R…)`),
所以任何 `pcall(一个无返回值的函数)` 解构成两个值都会被报。能让它变绿的改法只有两类:
加一个 `:: any` 强转,或者让 `walkScreen` 显式 `return nil` ——后者会让 `walkError` 的类型
变成 `nil`,而运行期它是错误字符串,**那是让类型说谎**。所以这一条留着。

- [ ] **把它接成门禁**。剩下的是两个决定,都不是「跑起来」那一步:
      - `modules/script/external/CMakeLists.txt` 现在把 `LUAU_BUILD_CLI` **强制关掉**并写了
        理由(Luau 只该有 VM + Compiler,不向顶层泄漏)。要构建 `luau-analyze` 就得掀掉这条。
        另外上游 0.730 有个 CMake bug:`LUAU_BUILD_CLI` 那段里 `set_target_properties` 引用了
        只在 `LUAU_BUILD_TESTS` 下才定义的 `Luau.UnitTest`,所以 CLI 开着就必须同时开测试,
        除非本地放一个同名占位目标接住那条属性。
      - 那 141 条 `Unknown global` 要怎么消。过滤掉它们是可以的——名单的权威在
        `framework-bundle.cpp`,检查器读同一份就不会漂——但这等于承认「bundle 就是
        definitions」,而不是真的给框架写一份类型声明。写声明能换来跨模块的类型检查,
        代价是它得跟着 15 个模块一起维护。**这两条都会长期留在仓库里,先定形状再动手。**

## P1–P3 后续

- [ ] P1:弹窗 interrupt、跨文件子任务、均匀缩放自适应、按需 OCR、标注批量维护与 confusion 诊断。
- [ ] P2:托盘 App、项目/任务入口、运行时浮层、HTML trace 报告、计划任务、portable/installer。
- [ ] P3:第二个游戏验证核心零游戏分支。

## 文字识别的准确率(2026-08-02 提出,待办)

标注一局出擊时读错的次数不少,但把当天每一次都归了类之后,**大多数不是识别器的错,
是取框的错**。分开记,否则「提升 OCR 准确率」会瞄准错的东西:

**取框错(当天四次,占多数)**

- `攻擊` 读成 `攻` —— 框下沿切掉了半个字。招募页三个职业标签都是,直接导致
  「三张都没有治癒/保護」的误判,而第三张明明是保護。
- `離開` 读成 `離開）` —— 框放宽之后把按钮**橙色圆角边框的弧线**框了进去,
  OCR 把那道弧认成全角右括号。**放宽读取框不是免费的**,圆角按钮尤其危险。
- `02/10`(牌堆计数)被当成一张手牌 —— 手牌区框下沿伸到了计数行。
- 名单页整列块读回来是碎片(`令 / 甘 / 00 / 等級 / 攻擊 / 60 / 友紀`)——
  215 宽的列框切到了相邻卡片的边缘,并把 UI 标签混了进来。

**识别器错(当天一次)**

- `休息` 读成 `休高`,置信度 6555 —— 禁用态的灰字压在队伍行李上,对比度低。
  **置信度地板 8000 挡住了它**,这一次机制起了作用:它没有变成一次错误的页面判定,
  而是变成矩阵里一条点名的 `unresolved_page`。

**不是错的那一类**

- `一縷光芒` 在手牌区读成 `縷光芒` —— 牌扇把每张牌的左缘压在前一张下面,
  第一个字**根本不在画面上**。这不是识别问题,是遮挡,解法是按片段匹配。

所以要做的事按这个顺序才划算:先让**取框**这件事不再靠肉眼(当天所有取框错都是在另一屏
上量的框拿到这一屏用),再谈换模型或调参数。第一条已经有工具了——存档截图加离线量框,
`camp_rest_*` 那两个元素就是这么标的,全程没碰真机。

- [ ] **把取框变成量出来的,而不是看出来的**。本节整段原本没有格子。四次取框错都是
      同一个动作:在另一屏上量的框拿到这一屏用。可做的形状:`cycle_read_lines` 已经
      把每行的文字**和它自己的矩形**一起给出来,所以一个离线动词能对着存档屏反推
      「这段字实际占哪个框」,作者不再自己估。注意两条已知代价:放宽读取框不是免费的
      (圆角按钮的弧线会被读成全角右括号),框下沿多一点就会把计数行吃进手牌区。
- [ ] **换模型或调参数**——排在上一条后面,今天没有证据说识别器是主要瓶颈(当天四次
      取框错、一次识别器错,而那一次还被置信度地板正确挡住了)。

## `project.load_project` 每次泄漏约 410 KB(2026-08-03 量到,当天修好)

这条取代 TODO 里此前对内存天花板的解释。当时归因于「chunk 之间没回收」,而
`5fae56f` / 探索会话每个 chunk 之间都跑完整 GC,宿主读到的数字是**回收之后的活集**。
真正的问题是加载工程会留下活对象。

实测(x64-release,chaos-daily 37 页 148 元素,每个 chunk 一次调用,宿主报的 `heap.used`):

- 对照组,chunk 里什么都不做:6 次全部 **1151796**,一字节不动。
- 实验组,每 chunk 一次 `project.load_project`:2317524 → 2910652 → 3201084 →
  3820788 → 4422060 → 4859756 → 5297500 → 5688116 → 6043964 → 6432532 →
  6788380 → 7258844。**每次约 +410 KB,单调不降。**

64 MiB ÷ 410 KB ≈ 150 次加载。当天一场真机探索会话在第 225 个 chunk 上以
`67104347 / 67108864` 结束,与此吻合。

~~**已排除的假设:铸造注册表。**~~ **这条结论是错的,2026-08-03 夜推翻。** 原话是:把十张
模块级表改成 `setmetatable({}, { __mode = "k" })`、重建 release、重测,得到「十次读数与
修改前逐字节相同」,于是判定这些表贡献为零。**逐字节相同本身就是那次实验失败的信号**——
一个真的生效了的改动不会让十个读数一位不差,而当时把它读成了「改动无效」而不是「改动没
生效」。历史留在这里,因为它是这个仓库最容易重犯的错:**拿一次空结果去排除一个假设,而
没有先证明这次实验有能力产出非空结果。**

**2026-08-03 已修。** 按上面那条待办逐段二分,每一步都有对照组:

- **分段二分(每段 4 个 chunk)**:`ctx:project_read` 四次全部 **+0**;`project.parse`
  第一次 +16384(分配器页粒度)之后 **+0 +0 +0**;`project.build`
  **+789496 / +392712 / +926424**,一次不落。泄漏全在 `build`。
  顺带证明了宿主报的 `heap.used` 确实是**回收之后**的活集:`parse` 也分配了整份文档表
  却完全不涨。
- **一个 chunk 内加载两次**:两次加载的 chunk 每次涨 **1302276**,一次加载的涨
  **716346**,比值 **1.82**。增长跟着**加载次数**走,不跟着 chunk 走——「首次加载建立了
  某个只增不减的结构」被排除,**每一次加载都留下自己的一份**。
- **让实验有能力产出非空结果**(上一轮缺的正是这一步):一个 chunk 里 `model.Element.new`
  铸 500 个元素,对照组是 500 个普通 `table.freeze`。对照组 **+0**;铸造组
  **+588984 / +588984 / +441768 / +409000 / +474536**——**约每个元素 1000 字节,永不释放**。
  这就是那次「逐字节相同」本该出现的信号。
- **改成弱键后**,同一个铸造实验:**+16384(一次)之后 +0 +0 +0 +0**。
- **完整 `project.load_project`**:修复前 12 次加载 1151796 → 7258844 单调不降;修复后
  13 次加载稳在 **~1220000**,以 ±16360 抖动、**有涨有跌**。

分两步做的,而且中间那一步本身就是证据:只把 `model` 的三张改弱,完整加载**照旧泄漏**
——因为 `Page.new` 铸的页面仍被强引用,而页面持有引用、引用持有元素,整张模型被页面钉住。
十张全改之后才归零。

弱键不削弱这些表守的性质:一条记录只在被铸对象**不可达**之后才会消失,而一个谁都够不到
的对象,也没人能拿它去骗过谓词。

- [x] 证伪测试:`tests/task/test-task-host.cpp`「what the framework minted is released once
  nothing can present it」——铸 500 个元素做基线,再铸 2000 个,断言堆读数增长小于 256 KB。
  强引用时这一段约涨 2 MB。
- 两条可复用的教训已提进
  [`pitfalls/embedded-vm-memory-ceiling.md`](pitfalls/embedded-vm-memory-ceiling.md):
  「记录身份的注册表会让它记录的东西活下去」与「空结果在证明实验能产出非空结果之前
  排除不了任何假设」。这里只留执行状态。
- 注意 `heap.used` 是 Luau 账本,C++ 侧的模板库和截图缓存不计入,所以泄漏一定在 Luau 侧。

## 两个 latch,explore 只看得见一个(2026-08-03 发现并修复)

已修:`script::Engine` 新增 `generationSpent()`,`ExplorationSession::terminalKind()`
取两个 latch 的并集,`finish()` 改成在销毁 VM **之前**问(引擎那半就住在 VM 里)。
证伪用例 `tests/task/test-task-host.cpp`「a chunk broken with no host call still ends
the exploration session」:`while true do end` 不碰任何宿主动词,所以只有引擎那半会置位;
拆掉并集这一半,恰好这一个测试转红,其余取消用例全绿——因为它们都是**穿过动词**取消的。
`operator-session.cpp:189` 查过了,那边没有 Luau VM,不存在第二个 latch,保持原样。
`entry/cli/explore.cpp:354` 那段注释现在是对的,不用改。

<details><summary>原始记录</summary>

- [x] `ExplorationSession::terminalKind()` 只读 `TaskContext::m_terminal`,那是宿主动词
  (`ffi/uf-tables.cpp` 的 `raiseCancelled` / `raiseInvariant`)写的。引擎自己的 latch
  (`script/ffi/engine.cpp:316-319`、`344-347`,在 `m_control.broken()` 后置位)传不到
  会话。于是被**墙钟或指令预算**打断的 chunk 会:引擎 latch 成 terminal,`explore.cpp:364`
  看不到,会话继续收 chunk,之后每个都拿到 `terminalRefusal`,而每次拒绝又刷新空闲计时
  —— 会话永不结束,且看起来还活着。
- 可达性不是理论的:`EngineConfig::interruptBudgetTicks` 默认一亿且**按 VM 世代累计**,
  explore 会话不覆盖它;`maxRuntime` 30 分钟同样不覆盖,对应真跑飞的 chunk。
- `entry/cli/explore.cpp:354-358` 的注释声称「会话在 latch 上结束」,对三个触发源里的两个
  是错的。这是代码漂移不是文档漂移,所以按 `correct-doc-drift` 的规矩记在这里而不是改注释。
- 修法方向:让会话的 terminal 判定取两个 latch 的并集(引擎需要暴露「这个世代已作废」),
  证伪测试用 stop token —— 撤掉修改后 `terminalKind()` 为空,测试转红。
  `operator-session.cpp:190` 和 `task-host.cpp:499` 同一模式,一并检查。

</details>

## 用会变的美术当锚点,是同一个坑的三次复发(2026-08-03)

三次都是同一形状:**拿一张会随内容改变的图去认一个不变的页面**,于是每见一个新内容
就得再采一张模板,永远差"下一个"。

- **boss 旗**:每个 boss 在同一面粉旗上画自己的图。采到第三面时停手——真正的判据是
  **尺寸 + 颜色**,不是图案。已由 `branch_arrow` 取代(见下)。
- **`node_badge`**:事件页左上角的徽记,一个事件一套美术,曾经从 4 种加到 6 种
  (`maw`、`horned`)。**已解决**(2026-08-03,见下)。
- 反例是 `branch_arrow`:每个分支画的是**同一个**三角形,所以一张色键裁片(19×24,
  bp 9848)覆盖所有类型;亮度状态只有三种,有界。旗子被它取代后,证伪矩阵从 132 秒
  超时降到 30 秒通过。

- [x] `event` / `event_node` 改锚点。**已完成**(2026-08-03 复核工程文件时发现已经做掉了,
  这条待办本身是过期的)。两页现在锚在规范说事件屏必画的两个控件上:右上角的加速控件
  `rest_speed` 与右下角的 `event_battle`。38 张存档屏上量到:`rest_speed` 命中 6 张、
  `event_battle` 命中 6 张、两者同时命中的**恰好 3 张**(event / event_node / rest_point)
  ——和旧的 `node_badge` + `event_battle` 组合命中的是同样三张,但**不再依赖徽记美术**。
  `event_node` 只要加速控件,因为 `event_battle` 那一行正是让 `event` 成为更窄那一页的
  东西。`node_badge` 已退役,工程文件里只剩注释提到它,没有任何引用。
  证据:`umbra-flow check --project chaos-daily` **findings=0**(2026-08-03 复跑)。

## 操作者的 Ctrl-C 大多被记成 `Failed` 而不是 `Cancelled`(2026-08-03 量到)

- [ ] 同一个 Ctrl-C,5 次真机测量里 **4 次 `runOutcome=Failed`、1 次 `Cancelled`**,
      差别只在哪条路先跑到。退出都很快(见上),所以这不是停机问题,是**记账问题**:
      一个数运行结果的消费者会把操作者的中断算成任务失败。
- 机制已查明,三段都在:
  1. `raiseCancelled`(`ffi/uf-tables.cpp:425`)**先**置 terminal latch 为 `Cancelled`,
     **再** `lua_pushstring(state, "uf: task cancelled")` + `lua_error`。抛出去的是一个
     普通 Luau 字符串,不带类型。
  2. `environment.cpp:314` 的分类器不认这个字符串,于是走兜底分支,`kind` 落成
     `InvalidResource`,报出来是 `InvalidResource: script error: uf: task cancelled`。
  3. latch 里那个 `Cancelled` 只在**运行自身没有失败**时才折进报告(`closeRunBracket`
     的注释写明了这条),而此刻它正好有一个——就是上面那条脚本错误。于是
     `TaskRunReport::outcome()` 看到的类型是 `InvalidResource`,判 `Failed`。
     走到「放弃任务线程」那条路的那一次没有脚本错误,latch 才折得进去,于是判 `Cancelled`。
- 要裁决的是**优先级**:宿主已经请求过取消,那么脚本展开时产生的句子还应不应该盖过它?
  「会话自身的失败盖过 sink 的失败」这条现有规则说的是 sink,没说取消。改法有两个方向——
  让取消的 raise 带上自己的类型(动 1),或者让 latch 里的 `Cancelled` 无条件优先(动 3)
  ——**这是一条会长期留在仓库里的规则,和 `capture_stalled` 那条同类,等你定。**

## 一次瞬时停帧就终结一整局 task run(2026-08-03 真机遇到)

- [ ] `capture_stalled` 让 `runOutcome` 直接变 `Failed`,一次已经投递 57 个动作的运行死在
  这里。而它是**瞬时**的:同样的停帧在探索会话里重试一次就恢复,今晚查明其中一次的诱因是
  游戏断线(屏幕上是「通訊不穩定。請點擊螢幕重新嘗試。」,已建模由脚本自己点掉重连)。
  但没有横幅的停帧(重动画期间)仍然会直接终结运行,脚本无从自愈。
- 要裁决的是宿主策略:停帧究竟是「这一帧没拿到」还是「这一代作废」。现在按后者处理。

## `run` 把任务的返回值丢掉(2026-08-03 发现,2026-08-03 修复)

- [x] `TaskHost::startTask` 改走 `runValue`,`TaskRunReport::returned` 带上渲染后的一行,
  `entry/cli/main.cpp` 在 `run:` 那行下面打印 `said: ...`。框架例程的 `answer` 仍是数字,
  从同一个值取 `number().value_or(0.0)`,`umbra-flow check` 的返回不变。
- 连带的语义变化:返回表或函数的任务现在**失败**(`InvalidResource`),而 `runNumber`
  时代它静默变成 0。这是 `runValue` 本来的立场——「一个结果行装不下的返回是失败,不是
  静默的空」——现在任务路径和 explore 路径一致了。
- 验收:`tests/task/test-task-host.cpp`「what a task returns reaches the report that
  describes its run」三个 SUBCASE(整句、什么都不返回、返回表)。

## 任务无法唤醒待机隐藏 UI 的目标(2026-08-03 量到)

- [ ] 主菜单闲置几秒后收起全部控件,只剩背景图,于是没有任何页 resolve 得了。任务的自救
  路径是断的:**唤醒需要输入,click 需要 receipt,receipt 需要有页 resolve** —— 而那正是
  被挡住的一环。实测(explore 会话,home 页):`cold=false`、移动指针 `move=false`、
  点击 `click=true`、按 `SHIFT` `after_SHIFT=false`。`ctx:key` 不要 receipt 却唤不醒,
  唯一唤得醒的 click 又要 receipt。
- 今晚是靠**第二个进程**在 run 那 20 帧容忍窗口里补投一次 `explore.click_point` 才起来的。
  顺带量到:两个 umbra-flow 进程可以同时绑同一个窗口,capture 与后台输入都不冲突。
- 可能的修法:(a) 框架给一个窄口径的唤醒动词,只在连续 N 帧无页可认之后可用;(b) run 启动
  时先投一次唤醒输入;(c) CLI 收一个 `--wake-point` 坐标。三者都得先回答同一个问题:在一
  个认不出任何页的帧上,凭什么允许投递输入——这正是 receipt 规则要拦的事。

## 项目任务脚本没有 require(2026-08-03 发现)

- [ ] 项目任务是 `<projectRoot>/tasks/<name>.luau` 单个 chunk,宿主全仓库没有 `require`。
  用户建模文档要求「不同的角色加载不同的出牌策略」,现在只能落成同一文件内的具名表。
  策略一多,单文件会撑不住。

## 延迟的健壮性台账

- [ ] 在 P0-C 前补遮挡、最小化/CaptureStalled、投递中 Ctrl-C 与 10–20 分钟长程验证。
- [`archive/plans/2026-07-20-post-port-win32-robustness.md`](archive/plans/2026-07-20-post-port-win32-robustness.md)
  —— HWND 复用竞态、best-effort Up、DPI 与 capture 取消边界。
- [`plans/2026-07-20-m0-demo-port-deviations.md`](plans/2026-07-20-m0-demo-port-deviations.md)
  —— 移植期有意偏差与后续清理项。
