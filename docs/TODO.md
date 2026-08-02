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
  投递边被拒(引擎不拒);`umbra-flow drive` 的操作者协议没有 scroll 动词。
  抓帧唤醒那条经验规则仍未移植进探索通道。

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
  裁决没有错,它只是**只在小矩形上付得起**;大搜索区域上今天没有第二条路,也没有
  任何东西在标注期把这个代价说给作者听。规则那一半记在
  [`pitfalls/element-choice-and-thresholds.md`](pitfalls/element-choice-and-thresholds.md)。

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
- [ ] **整局连续跑通**:未达成。逐页、逐步都验证过,一局从头到尾还没有落地过。
- [ ] **小地图那条路还没重标**:节点图标分不开(亮/暗/空 8885 / 8549 / 8582)。
      挡路的机制已于 2026-08-02 解除——`explore.crop` 收色键、把蒙版烘进模板 alpha,
      见上面那条已划掉的阻塞;剩下的是拿一个真机会话按节点自己的亮色重标一遍。
- [ ] 全程后台、不抢焦点;Unknown / StaleObservation 均 fail-closed 并留下可诊断 trace。
- [ ] 一局连续稳定跑完,Ctrl-C 500ms 内退出,单轮时长实测。

## Luau 代码规范(2026-08-02 测量完,一行未改)

仓库有 C++ 规范,**没有 Luau 规范**;`modules/task/runtime/*.luau` 那 15 个文件全靠习惯
维持写法。已按六个维度量过一遍,提纲、故意不裁决的六件事、以及按价值排序的 15 项改动
都在 [`plans/2026-08-02-luau-coding-standard.md`](plans/2026-08-02-luau-coding-standard.md)。
下面三条是量出来的**缺陷**,不是风格问题,单独列出来等批准:

- [ ] **`observe.luau:192` 的错误层级是错的(活 bug)**——`requireCtx` 抛 level 2,却被
  这个文件里 7 个公开 verb 调用,于是报错指向 framework 自己的源码而不是工程脚本里
  真正传错的那行。它下面二十行的 `readTarget` 用的是 3,注释写的正是前者违反的规则。
  全仓库没有任何测试断言过 error level。
- [ ] **整套不可变约定从未被证伪**——`table.freeze` 用了 37 次,把 `model` /
  `navigation` / `oracle` 里每一个全删掉,测试套件**仍然全绿**。`tests/` 里 14 处
  `isfrozen` 命中没有一处针对 Element / Page / Reference / Hit / Receipt / Edge /
  Graph / Claims。补一组「写入必须抛错」的对抗用例约 20 行。
- [ ] **`mint.frozen_extra` 是浅冻**(`mint.luau:106-115`,6 个调用点)——
  `Element.new{ extra = { tags = {...} } }` 冻出来的元素,`extra.tags` 还是调用方那张
  可写的表;而 `project.encode` 的 `renderValue` 把任何表都按数组渲染,嵌套 map 存盘时
  变成 `[]`。又一次静默删字段,正是 extra/residual 设计要防的东西。

规范的主干不是命名而是**线格式**:数据字段 `snake_case`、Luau 变量绑定永不 `snake_case`
(672 处字段 0 处驼峰),因为字段名同时是 TOML 键、脚本 API 和类型字段,拼错的后果是
`project.parse` 把它归进 `residual`、`project.build` 从不读它——文件看着没问题,
字段没了。这一行相对 C++ 规范是**反的**。

另外要先承认:今天没有任何机制检查 `.luau`——`fix_format.py` 的扩展名列表里没有它,
也没有任何类型检查跑过,所以 8907 行里每一个 `--!strict` 目前都只是带高亮的注释。

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
