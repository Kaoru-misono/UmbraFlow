# 状态层与策略插槽 — l2-v2 形状

> 状态:**方向已定,五项裁决已落(见第九节,2026-08-04 直答),按阶段待执行**。
> 2026-08-03 开发者在 `标注过程.md` 写下状态脑暴段并确认「按这个方向走」,
> 08-04 指示成文。上位文档:
> [标注模型重构](2026-07-31-annotation-model-capabilities.md)(能力集合与引用,不重开)、
> [script-owned](2026-07-31-script-owned-page-model.md)(模型住工程文件,不重开)。
> 证据基线:uf-chaos 2026-08-03 真机——85 步菜单到菜单的 `daily.luau` 运行
> (`frames/menu-to-menu5.jsonl`,21154 行 trace)与同日多次失败运行的教训。
> 本文每一节结尾给证伪方式;没有证伪方式的段落只是措辞,可以随便改。
> 工程仓库即 `uf-chaos`(2026-08-04 由 chaos-daily 更名,旧文档里的旧名指同一工程)。

## 一、真跑量出来的形状

普查对象:`uf-chaos/tasks/daily.luau`(1593 行,60 个 handler)与
`page-model.toml`(60 页、207 元素、240 引用、9 边、38 屏、76 expect 格)。

> **数字更正(2026-08-04 A 阶段实测)。** 上面那串是 08-03 那次普查的存量,写下时就已经
> 落后于文件。今天是 **87 页、331 元素、369 引用、9 边、85 屏、76 expect 格、27 模板**。
> 「60」不是过期的总数,而是**派发器覆盖的子集**:ORDER 恰好 60 个名字,与 60 个
> `HANDLERS.*` 逐名相等,87 − 60 就是那 27 个没有 handler 的 `season_*` 页。
> 于是矩阵是 85 × 87 = 7395 次解析、28985 格,不是本文原先算的 38 × 60 = 2280。
> 屏数从 71 变 85,是因为 A 阶段把 14 张只躺在 `assets/screens` 里、从未声明成
> `[[screen]]` 的帧注册了进来——在那之前**两边数目不等,`umbra-flow check` 直接拒跑**,
> 所以这份模型的证伪矩阵此前一次也没有跑起来过。

- **34 个 handler 的函数体就是一行 `act()`**——认出页,按一个按钮。每个都在编码
  一条出边,其中只有 6 页在模型里声明了边;**差 28 条**。
- **13 个 handler 是同一个算法**:读一块 → 按有序规则挑一个 → 点 → 可能确认。
  互相只差四个参数(offers 来源、规则表、兜底、要不要确认)。
- **真正独有的只有 3 个**:battle(回合循环)、fighter_list(滚动搜索)、
  node_map(小地图几何)。
- 派发器 ORDER 手排 60 个名字,自述「携带模型表达不了的锚点特异性」。把 60 页的
  identify-required 锚点集合两两求包含,**真子集只有 3 对**
  (deploy_ready⊂deploy、event_node⊂event、node_done⊂rest_done);其余 57 个
  位置不携带任何约束。而它举的例子 boss_result/battle_result 恰不是子集关系——
  那是一条关于某张截图上共解析的事实,今天没有任何检查在量它。
- 按 x 聚类"从帧上数张数"的代码写了三份,阈值 200 / 45 / 180 各自为政。
- **脚本自己长出了手写状态机**:`PRE_RUN_PAGES` + `runOver`(一局是一次任务)、
  `bossSettled`(没见结算不许脱逃)。两个都是真机撞了才补的。
- **投递后没人问"生效了吗"**:card_assign 对被浮层盖住的按钮点了 13 次;
  唯一做对的是 battle(以"那个牌名的张数掉了"为生效判据)。
- 08-03 日志挖出 25 页的观察后继;19 页只见过唯一后继,其中 15 页无边;
  且模型现有的 `sortie -(sortie_enter)-> deploy` 与实跑矛盾
  (第 1→2 步走的是 sortie → danger_variance)。
- `battle_ep` 标了 interact+read 从未被读;血量至今是裸矩形
  `[262,18,160,40]`,两处各抄一份。

**证伪**:以上全部可用仓外工程文件与日志复算;本文引用的每个数字都产自
2026-08-04 的脚本化普查,复算脚本一次性,不入库。

## 二、教义(与既有裁决对齐,不重开)

1. **模型只回答三问**:认(什么帧算哪页)、许(哪页允许对什么做什么)、
   变(做了会到哪)。证据两库:**截图库证认与许,轨迹库证变**。
   回答不了三问之一的是策略,住任务脚本。
2. **状态是信念,帧是真相**(navigation 原话照搬)。状态只收窄"先问哪些页",
   永远不禁问。反例现成:闪退回 home、网络浮层随时盖上来。
3. **页 vs 状态的判据**:动作集合不同就是两页(用 identify 的
   required/forbidden 区分,机制现成);动作集合相同、只是时机不同,是状态
   (外观门)。按此 draw_pick 拆两页(网格版/大牌版),rest_speed 档位、
   確認亮灰是状态。
4. **策略只选,不点**。每步四拍:读摘要 → 问策略 → 投递 → 验证生效。
   策略是纯函数,摸不到 ctx/周期/回执;投递与授权纪律在框架例程里写一次。
5. **不给 page 加 kind 分类字段**。分类会把今天的策略冻进只该放事实的地方;
   页面该用哪个例程,是任务文件的绑定,不是模型的属性。

## 三、schema:`umbraflow-project/l2-v1` → `l2-v2`

### 3.1 元素三形态

`fixed`(rect + 证据,今天的全部)、`shape`(只有模板,rect 免写)、
`strip`(容器 rect + 聚类间距,条目数从帧上数)。

- shape 消掉 map_* 五个死 rect(COVERAGE 五·3 已建议删,形态化让"删"变成"不许写")。
- strip 消掉 fate_card_1/2/3、rest_card_2/3 这类死重;三份聚类代码合一,
  间距成为模型里可对截图库证伪的测量值。
- **证伪**:regress 对 strip 增加一格——在声明的屏上按间距聚类,条目数与 expect
  声明的数目比对;把间距改错该屏必红。

### 3.2 外观门(state gate)

引用行新增 `interact_requires = "<appearance 名>"`:此页此元素的 interact 只在
该外观命中时授权。fate_confirm 采亮/灰两张模板后,「選擇卡牌之後會亮起才可点」
(标注过程.md 命运选择节)第一次可执行。Element 单一验证源的裁决不动:
带门的元素走模板,放弃读字。

- **证伪**:测试给出灰态帧,`observe.click` 拒绝;拆门后同测必绿转红向。

### 3.3 identify 的第三、四种证据

现有两种:模板、整窗文本相等。新增(框架实现,项目只能选,不接受自定义谓词——
observe 里"page signature 不是项目可按调用点放松的地方"的裁决不动):

- `expected_fragments = [...]`:有序片段依次出现,中间容洞。
  落地「最大HP…增加」(标注过程.md OCR 规则行)。
- `expected_presence = true`:读到非空且过 read_floor 即可。
  落地「范围内有文字即可」(事件节)。COVERAGE 二·1/二·2 两条整体销案。

### 3.4 page.state 与浮层族

- 场景页新增 `state = "<模式名>"`,模式名是工程自定字符串,框架不带枚举
  (P3 换游戏零改动)。uf-chaos 初版分区:`menu`(局外,含选人——两页的
  状态不值得单列)、`route`(node_map 枢纽)、`battle`、`event`、`camp`、
  `settlement`(结算尾)。(分区已裁决,2026-08-04 直答,第九节 3。)
- 浮层页不属于模式,声明 `over = ["battle", "event", ...]`(能盖在哪些模式上);
  奖励串在战斗后与篝火后都出现(08-03 日志 71-73 步),是族不是模式的直接证据。
- `interrupt = true` 补声明:network_retry(今天全模型零声明)。
- `catch_all = true` 补声明:dismiss_overlay。

  > **`interrupt` 与「兜底」是两件事,已拆开(2026-08-04 开发者裁决)。** 本条原先把
  > network_retry 和 dismiss_overlay 写在同一个旗标下。四·1 的矩阵量出它们的次序要求
  > 正好相反。
  >
  > `dismiss_overlay` 的 identify 锚点只有 `dismiss_hint` 一个(rect
  > `[655, 815, 300, 50]`,`expected_text = "點擊畫面可關閉視窗。"`)。它说的是「有个浮层,
  > 而且它说自己点一下就关」——是哪个浮层,它不知道。两张赛季页在这句之上各自还多一句:
  > `season_flash_cards` = `seasonflash_title` + `dismiss_hint`,`season_mental_intro` =
  > `season_mental_intro_anchor` + `dismiss_hint`。于是 `dismiss_overlay` 的子句集是两者的
  > 真子集,矩阵报的三对子集里正是这两对。再叠上五·1「interrupt 页永远在场、排在最前」,
  > 失效就是结构性的:凡解析出 `season_flash_cards` 的帧必然也解析出 `dismiss_overlay`,
  > 而后者先被问,这两页从此永远到不了。今天没坏,只因为 `tasks/daily.luau` 的 ORDER 把
  > `dismiss_overlay` 手排在最后一名——而那正是模式机要拿掉的东西。
  >
  > 裁决:**两个旗标,不合成一个。** `interrupt = true` 保持原义——随时可能盖满屏、
  > 必须先处理掉才谈得上继续的页,`network_retry` 是一个,先问它是对的。新增
  > `catch_all = true` 表示相反的次序——签名描述的是一**类**画面而不是某一屏的页,
  > 它同样永远在场,但要**最后**问,等每一张具体页都问过之后。`dismiss_overlay`
  > 从 `interrupt` 移到 `catch_all`。这不是新道理:[WORKLIST](../WORKLIST.md) 一·2
  > 记的六条派发器护栏里就有「事件页做**兜底**而不是候选 —— 结构上杜绝假阳性」,
  > 本条只是把那一条从手排纪律提升成模型里的声明。
  >
  > **字段名取 `catch_all`,弃 `fallback`**:`fallback` 是「兜底」的直译,但六节的策略表
  > 已经用它表示「没有规则命中时做什么」,同名不同义会读岔;`catch_all` 直说签名捕的是
  > 一类而不是一屏。
  >
  > 两条后果一并记:
  >
  > 1. **候选次序从此是推导出来的,不是手排的。** interrupt 先、具体页居中、catch_all
  >    最后,这三档写进模型之后,五·1 那份候选层次不再是一张要维护的名单,而是每页各自
  >    声明的结果——这正是四·1 承诺过的「取代 ORDER 的 60 个手排名字」。
  > 2. **B 阶段该加一条检查,而且很便宜。** 锚点子集格是纯文件算的(四·1,零抓帧),
  >    所以 regress 当场就能判:一页声明了 `interrupt`,又站在子集表的**被包含**一侧,
  >    就报一条 **finding**——包含它的那一页从此永远解析不到。不必再附加「没有 `forbidden`
  >    子句把两者分开」这个条件:四·1 已经论证过,极性是子句键的一部分,真能仲裁的那种
  >    配置根本进不了这张表,凡进表的都已经满足。是 finding 不是行:普通共解析只是事实,
  >    只报不判;这一条是模型自己就能断定的缺陷。
  >
  >    > **已实现(2026-08-05):`regress` 的 `interrupt_swallows`。** 判据如上,并且
  >    > `catch_all` 在 general 一侧是**修复而不是同一缺陷的第二种写法**——它说「最后问」,
  >    > 正是包含关系需要的次序,所以那种配对不报。没有任一旗标的包含仍然只是行:派发器
  >    > 先问具体页是正常安排,不是缺陷。
  >    >
  >    > 工程仓库现在**一条都不报**:唯一声明 `interrupt` 的 `network_retry` 不在任何
  >    > 子集对的被包含一侧。这是阴性结果,而阳性对照在测试里——同一段用例先造出会报的
  >    > 配对,再把旗标换成 `catch_all` 证明它转绿。
  >
  > 顺带一记:矩阵报的第三对 `event_node ⊆ event` 形状相同(`event_node` 只必需
  > `rest_speed`,`event` 还要 `event_battle`),今天也靠 ORDER 排开。它是 `catch_all`
  > 的下一个候选,但本次未裁决。

- **证伪**:trace 回放(四·2)断言"处于模式 X 的区间内,未解析出属于其他模式的
  场景页";08-03 那 85 步是第一份底卡。

### 3.5 边补全与围观边

- 从轨迹库把 28 条一键边补进模型;修正 sortie 的 `to`。

  > **已补,9 条 → 31 条(2026-08-05)。** 来源不是 trace 而是
  > `frames/daily-log.txt`——08-03 那次运行读的模型已经不存在,回放喂不进去(见第八节
  > A 阶段的注)。口径按最严的取:`<no page resolved>` 断链、自环丢弃、只有投递过的步
  > 才能作为起点。46 条原始转移过滤后并成 27 条,其中 3 条确认了既有边、1 条与既有边
  > 矛盾、23 条是新的。
  >
  > 挖的办法值得记一笔:日志每一行都是 `daily.luau` 里的一个字符串字面量,所以不是去
  > 解析散文,而是把每条注解反查回发出它的 handler。由此"这一步点了哪个元素"才是可答的
  > ——`actThenConfirm` 那类的触发元素是**确认键**不是卡片,而 `battle` 根本不是点击,
  > 回合结束在 `ctx:key(ending, "E")`,所以它是本模型第一条 `via = "key"` 的边,
  > 一条边带三个 `to`。
  >
  > **「离开浮层 = pop」不成立,这是补边最值钱的产出。** `card_taken`、`skip_confirm`、
  > `camp_confirm` 三个浮层确认掉之后**不露出它们盖住的那一页**,而是把流程往前推:
  > 前两个盖着 `card_reward` 却落到 `node_reward`,第三个盖着 `campfire` 却落到
  > `node_reward_done`。而 `walk_edge` 把 pop 的目的地算成栈顶下面那一个然后等它——
  > 按 pop 写,这三条边会在真机上等一个永远不来的页面然后超时。三条都改写成 `navigate`
  > 加显式 `to`:`Edge.new` 只查目的地的 overlay 旗标、从不查来源页的,所以从浮层
  > navigate 出去本来就合法;而 `believe_arrival` 在任何非浮层到达时把栈重置为底座页,
  > 栈的语义也不因此打折。
  >
  > **`extreme_intro -(overlay_confirm)-> rest_point` 只删不换。** 那条确定是错的
  > (`rest_point` 在 85 步里零解析,且九·2 已裁决它退休),但观察到的后继紧跟在一个
  > `<no page resolved>` 之后,`event_node` / `event` / 两者皆有三种读法都站得住。模型里
  > 少一条边只是"未知",留一条猜的是"错误"。等回放检查器加真机跑一局,它会自己补上——
  > 这一条正好是那个检查器存在的理由。
  >
  > 两条证据薄的仍然进了模型,理由各自不同:`event -(event_offers)-> [dice_roll]` 是
  > 2:1,而那 1 次的帧自己 raise 了 `recognition_incomplete`(读不到任何文字);
  > `dice_roll -(dice_surface)->` 是 pop,pop 不声明 `to`,所以证据的瑕疵影响不到记录内容。
- 新增边旗标 `preview = true`(围观边):执行它**不换页**——点 roster 行只换
  预览区这类。运行时合同:围观投递后本页必须仍解析,否则例程按假设破裂上报。
- **证伪**:回放检查里,每个观察到的页转移要么命中一条边要么出 finding;
  围观边则断言"trace 里它的投递之后,下一次解析仍是本页"。

### 3.6 不动的

wake_point 与其"证明什么都不按"的矩阵检查;能力集合;Holding/exercised;
screen/expect;残余段落保序往返。schema 串升 `l2-v2`。

> **B 阶段落地情况(2026-08-05)。** 三·2 外观门(`interact_requires`)、三·3 两种新证据
> (`expected_fragments` / `expected_presence`)、三·4(`state` / `over` / `catch_all`)、
> 三·5(`preview`)、三·1 的 **strip**(`item_spacing` + `observe.read_strip`)与本节的
> schema 串**都已落地**,每一项一对红绿。工程文件已迁移到 `l2-v2`。
>
> **两件明写没做的**,免得被当成做完了:
>
> 1. **strip 的 regress 格**。本节承诺「在声明的屏上按间距聚类,条目数与 expect 声明的数目
>    比对」,但 oracle 今天没有承载那个数目的声明字段。所以 strip 的间距是在文件里断言、
>    没有任何屏幕检查它。
> 2. **三·1 的 `shape` 形态**。「只有模板、rect 免写」今天已经成立(rect 本来可选)。要把
>    「删掉死 rect」变成「不许写」,需要元素上一个显式且必填的 `form` 字段——按
>    「Break it rather than bridge it」它不能有缺省——于是工程仓库 331 个元素都要迁移,
>    而迁移的内容是把文件已经说过的话再说一遍。**这个取舍留给开发者**:代价是少了「这里
>    故意没有 rect」和「有人忘了写 rect」的区分。

> **更正(2026-08-04):`project.parse` 从不拒绝未知字段。** 未知顶层键进
> `document.preamble`,已知段落里的未知键进该段的 `residual`,未知 `[[段落]]` 进
> `document.blocks`,三者都原样往返写回。拒绝未知键的是 `mint.unknown_key`,作用在
> **构造器的 spec 表**(`Element.new` / `Page.new` / `Edge.new` / `Screen.new` …),
> 是另一张面。B 阶段若照原话去"保持"一个从不存在的拒绝,就会砸掉 residual 往返——
> 而那正是这套文件格式存在的理由。
>
> ~~这条反过来是 B 阶段可以用的:今天就能往 `[[edge]]` 里写 `preview = true`,它会
> 逐字节往返,只是没有人解释它。先写数据、后接语义是可行的。~~
>
> **撤回(2026-08-05)。** 「先写数据、后接语义」正是 CLAUDE.md「Break it rather than
> bridge it」禁止的那种分期上线:文件里躺着一个没人解释的键,读的人分不清它是待接线的
> 新字段还是手滑打错的旧字段。要 `preview` 就在同一次改动里把 `Edge.new`、
> `project.parse`/`encode`、运行时合同和红绿测试一起做完。
>
> residual 往返本身**不受影响**,它保的是「这个 build 看不懂的字节别删掉」——工程文件是
> 手写的,而且住在另一个仓库。那跟「同一件事接受两种拼法」不是一回事。

## 四、证据两库与新检查

### 4.1 共解析矩阵(截图库,regress 新检查)

> **已有一个可复现实例(2026-08-04)。** 在赛季 `靈光一閃卡牌` 那一帧上,
> `dismiss_overlay` 与 `season_flash_cards` **同时解析成功**——两者共用同一句
> 「點擊畫面可關閉視窗。」。这条以前是论证,现在是数据。反向的例子同帧也有:
> 日常 `node_map` 在赛季**三分支**路线页上解析、在**单分支**布局上不解析,
> 说明它的锚点不是那一页的常量。两者都该由本节这张矩阵自动说出来。

85 张屏 × 87 页全解析。产出三件:谁与谁在同屏共解析(派发内序的**全部**真依据,
取代 ORDER 的 60 个手排名字)、锚点子集关系、从未在任何屏上解析成功的页
(覆盖缺口清单——87 页对 85 屏,缺口本身是发现)。

> **已落地并跑出第一份报告(2026-08-04)。** `regress.check` 里加了
> `recognition.sweep`(骑在每屏那唯一一次观察上,不多开一帧)与
> `recognition.anchor_subsets`(纯文件、零抓帧),verdict 多出 `resolution` /
> `anchor_subset` / `page_coverage` 三种行(08-05 又加第四种 `page_linkage`,见
> 四·2),**全部只报不判**——`accepted` 仍然只数
> findings(裁决见[三层文档](2026-08-01-three-layers-and-agent-operator.md)
> 2026-08-04 一节)。量出来的:
>
> - **`season_event` 在 85 张屏里的 59 张上解析成功。** 它的签名是必需
>   `seasonevent_crest` 加禁止 `battle_draw`,而 `seasonevent_crest` 正是
>   [WORKLIST](../WORKLIST.md) 一·1 点名的无掩膜模板之一。那一条原本是论证,现在是
>   一个数字:**这不是页面签名,是常量**。同族还有 `event_node` 命中 10 屏、
>   `fighter_list` 8 屏、`dismiss_overlay` 5 屏。
> - 85 屏里 **62 屏有不止一页解析**;3 条锚点子集(`dismiss_overlay` ⊆
>   `season_flash_cards` / `season_mental_intro`,`event_node` ⊆ `event`),今天只靠
>   ORDER 排开。**加一条 forbidden 守卫也解除不了这个顺序依赖**:包含关系一旦成立,
>   能解析出 specific 的帧必然也解析出 general,先问 general 就永远到不了 specific,
>   多出来的那条子句是必需还是禁止都一样。真能仲裁一对页的配置是 general **禁止**、
>   specific **必需**同一个标记——而那种配置一行都不会出现在这张表里:极性是子句键的
>   一部分,包含判定当场不成立(`recognition.identifyClauses`)。
> - **两半都不可省。** 子集是纯符号的,`event`/`event_node`/`rest_point` 在
>   `rest_point` 那张屏上三页齐鸣,而它们的锚点名两两不交——子集分析看不见它;
>   反过来像素扫也说不出"谁包含谁"。本节原先只写了像素那一半。
> - 18 页在任何屏上都不解析,**其中 0 页是"有屏但签名不成立"**——18 页全都是从未
>   存过屏。`page_coverage` 行同时报 `resolved_on` 与 `declared_screens`,就是为了
>   让这两件事不混:一页没被拍过和一页认不出,是两个结论。

### 4.2 轨迹回放(新的离线检查,不开真机)

输入一份 run trace,验:每个页转移命中一条边;模式区间纯净(3.4);围观边不换页
(3.5);投递后的生效期望(五·2)。跑一局就多一份底卡,与 expect 矩阵互为镜像:
**截图库让"认/许"可证伪,轨迹库让"变"可证伪**。

> **输入有了,检查器还没有(2026-08-04)。** 新增了一个 additive 事件
> `framework.page_resolved`:`observe.resolve_page` 铸出票据时由框架发出,只带页名,
> 成功才发、失败不发。所以从今天起每一次运行都自动留下"它认为自己依次站在哪几页"
> 这条序列,本节第一件要验的事不再没有料。**回放检查器本身仍然不存在**——这里改的
> 只是"没有输入",不是"做完了"。它必须遵守的一条约束见八·A 的进度注:按
> `run.started.frontEnd` 排除 check 自己的轨迹,而不是按任务名。

> **连通性那一半不用等轨迹,已落地(2026-08-05)。** 本节把「页转移命中一条边」记成回放
> 才能问的事,但它有一半是纯文件的:哪些页**根本没有任何入边**,不看任何一次运行就能算。
> 这半已经进 `regress`,与锚点子集同一档——零抓帧,只报不判。verdict 多出 `linkage` 行
> (`page_linkage`:每页的入边数、出边数、是否 `interrupt`)与 summary 的 `pages_unlinked`。
>
> pop 不计入任何页的入边:pop 落在被关掉的那一页**下面**,那是运行时的栈知道而文件不知道
> 的事(`navigation.Edge`)。所以「只靠关浮层才能到达」的页在这里读作零,这是诚实答案而
> 不是缺陷。`interrupt` 页也不计入 `pages_unlinked`:它一次性声明了「随处可盖」,没有任何
> 一条边能消掉它的零。
>
> **自环不算入边。** 走一条自环的前提是已经站在那一页上,所以它是"离开的方式"而不是
> "到达的方式"。反过来算的代价是具体的:`battle` 今天正是"有出边无入边"四页之一,一旦
> 有人把"按 E 结束回合仍留在 battle"建成 `battle → battle`,它就会从未连通清单里消失,
> 而它仍然没有任何进入方式——那正是这行要防的事。
>
> uf-chaos 实测:87 页里 **60 页没有入边**,排掉唯一声明 `interrupt` 的 `network_retry`
> 之后 `pages_unlinked` 报 **59**。其中 **4 页有出边而无入边**(`battle`、`equip_assign`、
> `fate_choice`、`flash_result`:运行离得开,文件却没说怎么到),**56 页一条边都不沾**
> (排掉 interrupt 是 55)。边的构成是 27 条 navigate + 4 条 push,零 pop、零自环。
> summary 其余字段与本次改动前逐字段相同。
>
> 另记一条底噪:模型里没有"入口页"这个概念(`wake_point` 是坐标不是页),所以任何诚实
> 模型的起始页都会被算进 `pages_unlinked`,这个数的地板是 1 而不是 0。
>
> `network_retry` 的 `interrupt = true` 是本次按三·4 补的,而**它对 uf-chaos 今天零行为
> 变化**:这个旗标的运行期消费者只有 `observe.walk_edge` 的超时兜底,而 `tasks/daily.luau`
> 与 `tasks/idle-cycles.luau` 一次都没用过 `navigation` / `walk_edge`——两个任务都走自己的
> 手排派发表。补它是对的(声明属实,且模式机要靠它),但理由是"文件说了真话",不是
> "运行时立刻受益"。它今天唯一可见的效果就是把 `pages_unlinked` 从 60 压到 59。
>
> 门已验(证伪而非正例):去掉 `linkPages` 调用,三个 subcase 全红;只去掉 summary 里
> 对 `interrupt` 的排除,只有第二个红;把自环算进入边,只有第一个红;把 `linkPages`
> 关进 `if swept`,只有第四个红。
>
> **回放检查器本身还缺两块地基,但都比先前记的这段浅——两条原话都是错的,已更正。**
>
> 1. **读 JSON 不必从零开始。** 原话说"仓库没有任何 JSON 读取器、也没有 JSON 解析设施",
>    两句都假:`modules/trace/source/trace/event.cpp` 里就有 `skipString` / `skipValue` /
>    `findTopLevelMember`(`stripNonGoldenFields` 按成员名从已渲染的 trace 行里裁字段),
>    `entry/cli/explore-protocol.cpp` 的 `LineReader` 是一个严格的逐行 JSON 对象读取器,
>    它的注释还专门论证过为什么不与另一个共用。**缺的是"JSONL → TraceEvent"这一层**,
>    不是 JSON 本身。所以这里大概率不需要走 `evaluate-core-capability` 加 core 设施,
>    先看这两处能不能长出第三个窄读取器。
> 2. ~~**模型身份判得了,只是没有上线。**~~ **已上线(2026-08-05)。** 原话说"host 根本
>    不读 `page-model.toml`,所以模型哈希不在 host 手上",假:`TaskHost::loadProject` 在
>    任何 VM 存在之前就调 `readPageModelFacts`(`task-host.cpp:636`),整份文件已经读进
>    内存做扁平行扫。它取的 `ProjectFingerprint` 是**分辨率**而不是内容哈希,所以线上
>    此前确实没有模型身份;但字节在手,而同一个 host 已经在 `task-loader.cpp:167` 对
>    脚本源码调 `sha256`。
>
>    于是走了最省的那条:`PageModelFacts` 多一个 `contentHash`,在 `parsePageModelFacts`
>    里对**传进来的字节**求一次 sha256;`run.started` 多一个 `modelHash` 字段,紧跟在
>    `sourceHash` 之后。没有新事件,没有新的 schema 版本——`umbraflow-trace/v4` 不动,
>    这是 additive 字段。uf-chaos 实跑核对过:trace 首行的 `modelHash` 与
>    `Get-FileHash page-model.toml` 逐字节相同。
>
>    **哈希取在字节上而不是取在解析结果上**,这条是有意的:那个扁平行扫只认段落头和
>    `name` 行,`page-model.toml` 里绝大部分内容(阈值、引用、边、`[[expect]]`)它根本
>    不看;取解析结果的哈希会让"只改了边"的两份模型算成同一份,而回放检查器要对的恰恰
>    就是边。已用一对红绿钉住:把哈希改成只覆盖文件前 16 字节,身份测试与 wire 金线
>    两处同时红。
>
>    于是四·2 承诺的"模型哈希不符就拒跑"从今天起有料可判。**回放检查器仍然只差
>    JSONL→事件那一层**(见上一条)。
>
> 另外把今天能验的范围说准:三·4 的模式区间纯净要 `state` 字段、三·5 的围观边不换页要
> `preview` 旗标,两者都是 B 阶段的 schema。**回放检查器今天能实现的子集是**:页转移对边、
> 无边可解释的点击(`unattributable`)、连通性(文件那半已如上落地),加上按 `frontEnd`
> 排除自己。

> **两块地基都补齐了(2026-08-05)。检查器本身仍未写,但它现在不缺任何输入。**
>
> 1. **JSONL → 事件那一层落地了**,`trace::readReplayedRun`。它是**投影不是解析**:一条流
>    几十种事件,回放只问「信了什么、做了什么」,所以只投影页解析、点击授权、点击投递、
>    按键投递四种。扫描器复用 `event.cpp` 原有的那一个(挪进 `json-scan.hpp`),没有第二
>    份对同一格式的意见。它什么都不判——check 自己的轨迹读回来就是 `Check` 而不是被拒,
>    模型哈希对不对是检查器的裁决。**结果是不必往 core 加通用 JSON 设施**,先前那条担心
>    是建立在「仓库里没有 JSON 读取器」这个错判上的。
> 2. **点击可归因了。** 新增 additive 事件 `framework.element_clicked`,由
>    `observe.click` 在授权之后、投递之前发出,只带元素名。
>
>    **只带元素名是有意的**:授权页由协议保证——`requireAuthorisedHit` 已经拒绝了别的
>    周期铸的票据和别的页的票据,所以授权页必然是本次观察最后解析出的那一页。再写一遍
>    只会多一个可能与事实不符的字段,而不是多一个事实。
>
>    **写在投递之前也是有意的**:这行说的是「这次点击获授权打在哪个元素上」,至于它有没有
>    真的送到目标,是 engine 下一行自己的事。写在之后的话,恰恰在投递失败的那些运行里
>    这行会缺席——而那正是读的人最需要它的时候。
>
>    **`long_press` 不发这行。** 边的触发在 schema 里只有 click 和 key 两种拼法,给一个
>    没有边能引用的长按记一个元素名,是关于任何事都不成立的证据。
>
>    **它被拒于 check 与 annotation 流之外**,而 `framework.page_resolved` 不是。判据是
>    这行说「一个**页授权**的元素被点了」,而唯一通过页模型点击的前端就是跑任务的那个:
>    探索会话点的是裸坐标、写成 `annotation.click_delivered`,check 什么都不投递。
>
> 于是四·2 承诺的那些断言里,**页转移对边**与**无边可解释的点击**从今天起都有料。

> **检查器落地了(2026-08-05):`replay.luau`,`replay.check(built, steps)`。**
> 它是 `regress` 的镜像——那个问模型声称的**屏**是否说了它声称的话,这个问一次运行做过的
> **动作**是不是模型画得出的边。零抓帧、不接 ctx,所以在没有目标的机器上也能跑。
>
> 判据四条,每条都定死了:
>
> - **命中一条边** → 行 `matched`。
> - **pop 命中但不查落点** → 行 `matched_pop`。pop 落在哪是运行时的栈知道、文件不知道的
>   事,所以对落点表示"一致"等于对没人写下的东西表示一致。
> - **走了模型画不出的路** → **finding** `no_edge`,并且分两种措辞:「这一页在这个触发上
>   根本没有出边」和「有这条出边但它落在别处」——两者修法不同,一句笼统的拒绝会把它们
>   读成一件事。
> - **没有名字的点击** → 行 `unattributable`,**永远不是 finding**。没人能拼出来的动作
>   不构成对模型的反证;把它算成缺边会让报告塞满多半确实存在的边。长按、以及
>   `framework.element_clicked` 之前录的流,整条都读作这一类。
>
> 自环不产生转移:页没变就没有要解释的移动。`accepted` 仍然只数 findings。
>
> 门已验:把「没有名字的点击」改判成 finding,红;让 pop 去查落点,红。
>
> **`umbra-flow replay --project DIR --trace PATH` 通了(2026-08-05)。四·2 落地。**
>
> 投影**作为数据**经 `ctx:replay_steps` 进 VM,不是拼进例程源码。例程的其它参数确实是
> 格式化进 Luau 文本的,但页名是工程自己的字符串——工程的字符串一旦成为程序的一部分,
> 这个工程就能改写检查它的东西。
>
> 两道拒绝在 host 侧,VM 启动之前:
>
> - **前端不是 `task` 就拒**。真实验证:拿一份 19127 行的 check 轨迹去 replay,被按前端
>   拒掉;把它改成 `"frontEnd":"task"` 再喂进去,报出 **186 条 no_edge**——193 次解析、
>   0 条命中。那不是关于模型的证据,那正是「把一次 sweep 当成一次 walk 读」的样子,也就是
>   这道拒绝存在的理由。
> - **模型哈希不符就拒**。它**只能**在这里判:哈希根本到不了脚本层。
>
> 这次运行自己也写一条流,前端是 `Check`(它什么都不投递),写在固定文件名而不是输入轨迹
> 旁边——否则第二次 replay 会盖掉第一次要检查的证据。绑的 frame source 与 action sink
> 全部拒绝:回放判的是已经发生过的运行,此刻能抓到的任何一帧都是那次运行没见过的屏。

### 4.3 语料管理(回答"screens 要不要进版本管理")

- **入库**:`page-model.toml`、`tasks/`、`assets/templates`(**200 KB / 27 张**,
  2026-08-04 实测 199889 字节;本文原先写的 240 KB 没有测量作依据)、三份 md 台账。

  > **`assets/screens` 已于 2026-08-04 出库,本条原先的裁决就此更正。**
  > 原话是「不入库的模型是不可证伪的模型」——这一步是错的。可证伪性挂在
  > **模型里那 85 条 `[[screen]] hash`** 上,不挂在文件躺在哪里:文件名就是内容哈希,
  > 那份清单本身就是 lock,`umbra-flow check` 在目录与模型不一致时已经会拒跑
  > (今天亲见:「declares 71 screens and its directory holds 85」)。
  > 出库后重跑矩阵,summary 与出库前**逐字节相同**。
  >
  > 反过来,git 为这批数据买到的东西几乎为零:它内容寻址、一次写入永不改写,
  > 所以「防误改」是它自己的命名规则already提供的;而工程仓库**没有 remote**,
  > `.git` 与工作区同盘,「入库」连一份备份都不是。代价则是同样的字节存两遍——
  > 出库并重写历史后 `.git` 从 **148 MB 降到 413 KB**,23 个提交一个没少。
  >
  > **备份另行安排,且开发者 2026-08-04 裁决暂不做**:语料只有本地一份,可接受。
- **不入库**:`frames/`(运行输出)、`run-trace.jsonl`(每局重生成)、
  `*.before-*`(手工备份,入库后由 git 史替代)、`templates-unused/`(裁决后删)。
- **有条件入库**:被 4.2 引为证据的 trace,精馏成"解析与投递事件"子集后入库
  (21154 行原始 trace ≈ 9 MB,精馏后应在数十 KB);原始件压缩归档在库外。
- E:\umbraflow-projects\uf-chaos **已 git init**(2026-08-04,连同 `.gitignore`
  与关掉换行改写的 `.gitattributes`)。根提交原为 `775ae2d`;同日把 screens 清出
  历史后全部提交哈希重写,现在的根是 `f073944`,头是 `3759cb9`。

## 五、运行时:模式机与例程层

新 runtime 模块(文件名=全局名,进 frameworkProjectGlobals;evidence 不发布的
裁决不动):

### 5.1 模式机

信念变量一枚。翻转只由观察驱动:**看见下一模式的场景页解析成功才算进入**,
按下"進入"不算——与回执同帧纪律同源。候选次序三档:`interrupt` 页永远在场且最先问
→ 当前模式的场景页 + 声明盖在此模式上的浮层 → `catch_all` 页最后问;连续 N 帧全不
解析时放宽到全集,再不行走 wake。每步候选从 60 降到十几,周期 32 次读的预算直接宽松。

> **这三档是推导出来的,不是排出来的(2026-08-04,裁决见三·4)。** 本节原先写「候选
> 三层」,把 `dismiss_overlay` 这类只描述一类画面的页归在 interrupt 里一起先问——那会
> 让比它更具体的页永远解析不到。`catch_all` 拆出来之后,这里不再是一张要维护的名单:
> 每页自己声明属于哪一档,次序是声明的结果。这就是四·1 说的「取代 ORDER 的 60 个
> 手排名字」。

### 5.2 四拍 step 与生效验证

投递后必须验生效,判据按例程:walk 验"到了 to 页"(walk_edge 的
consecutive/timeout 现成);choose 验"计数动了或页走了";turns 验"牌名张数掉了"。
card_assign 那 13 次点在被盖住的按钮上,今后在第 2-3 次就被"没生效"截住,
而不是靠派发器 REPEAT_LIMIT=12 在错误层级兜底。

### 5.3 例程

- `walk`:**未绑定且恰有一条非围观出边的页,默认走边**——34 个一键 handler
  连声明都不用写。零条或多条出边则诚实拒绝(与 wake 同款)。
- `choose_one`:strip 读 offers + 计数闸(n/m 不足不许确认)+ 外观门 + 确认前
  重解析。13 个"挑一个" handler 变 13 张表。
- `turns`:battle 骨架——两阶段基本牌最后、成功清 blocked、张数验证、九键上限、
  取消判据。骨架是"任何牌怎么被安全打出",换角色零改动。
- `scroll_search`:滚动到规则命中(fighter_list 的形状,通用)。
- `inspect_then_commit`:围观边逐个看 → 策略一次决定 → 单点提交。
  同一次到访内缓存围观答案,重复问不重复点。
- node_map 这类真特异的仍写自定义 handler,用 observe 动词,纪律不破。

### 5.4 摘要合同(策略的输入)

- `feeds`:情境声明要读什么(strip/计数/量表都是模型元素),框架**同周期读齐**
  ——摘要内所有事实属于同一帧,策略永远不会拿上帧的 EP 配这帧的手牌。
- `knowledge`:策略表自带的不变知识(牌名 → 费用/标签/优先级),不占读预算。
- `memory`:跨回合自有状态,决定本身保持纯。
- **问答协议**:策略返回"决定"或"问题";框架用 feeds 或围观边回答问题,
  垫进摘要再问。复杂策略(先看各角色现有装备再定覆盖还是提炼)由此成立,
  而"策略摸不到 ctx"的线一毫米不动。

### 5.5 回测

纯策略 + trace 里的读数 = 离线重放:改一版规则表,对 08-03 那局重放,看哪几步
选得不一样,不开真机。**新策略问了老运行没问过的问题,回测必须报"此支无数据"**
——没有阳性对照的阴性结果不算排除(pitfalls 已有此条)。

## 六、任务文件收缩后的样子

```lua
local policies = {
    card_reward  = strategy.ranked { rules = REWARD_WANTED, fallback = "skip" },
    event        = strategy.ranked { rules = EVENT_WHITELIST, never = EVENT_NEVER,
                                     fallback = "first_that_ends" },
    equip_assign = strategy.ranked { rules = {},          -- 空槽,空得可见
                                     -- 已裁决(九·1):围观现有装备,空槽优先,
                                     -- 都满才提炼;D 之前的行为只是临时的。
                                     fallback = "empty_slot_else_refine" },
    route        = strategy.ranked { rules = ROUTE, when_hurt = ROUTE_HURT },
    battle       = strategy.turns  { feeds = {...}, knowledge = CARDS, decide = ... },
}
return task.run_project(ctx, { policies = policies,
                               rules = { one_run_per_task = true,
                                         escape_needs_settlement = true } })
```

空槽是诚实的:card_assign 今天的 "skipped (no policy)" 和装备那条只活在注释里的
"等級最高",都变成表里看得见的一行。两个手写布尔升格为声明的 rules。

## 七、uf-chaos 三个实装样例

- **battle**:feeds = 手牌 strip + battle_ep(已标注,首次接线)+ HP 元素
  (裸矩形转正);knowledge 按**牌名**为键,多角色表装载时合并,局内招募的牌
  自动生效,表外的牌落骨架"从右往左、基本最后";「受伤打支援」从换整表变成
  条件(hurt 时某些牌优先级抬升),一张表两种打法。
- **装备**(流向按开发者 08-04 澄清):两件供选时**先在 equip_pick 选装备,
  选完才看到当前角色的装备**。所以 pick 一步只有 offer 文本 + knowledge 可用;
  gear-aware 的部分全在 equip_assign。兜底已裁决(九·1):空槽优先,都满才提炼。

  > **围观边不需要(2026-08-04 赛季实测,`uf-chaos/SEASON.md` 四·1)。** 本文原先假设
  > 要点角色行才能看到他现有的装备。赛季的装备分配页证伪了这个假设:**三个战斗员各自的
  > 三个槽(武器/防具/饰品)就画在同一帧上**,空槽是暗的,本次要装的那类描金边,`推薦`
  > 徽记跟着行走。所以这条裁决在一次观察内可判,装备走 `choose_one` 加厚摘要即可,
  > `inspect_then_commit` 留给真正需要多帧的场合(目前没有已知实例)。
  待真机测一件事:equip_pick 上选中一件后能否撤回换看另一件;可撤则 pick 也
  升级为围观式比较,不可撤则维持"文本规则 + 首件兜底"。
- **选路**:hurt 翻转成条件;小地图行序与分支序的对应仍未量
  (COVERAGE 已记,不因本文改变状态)。

## 八、阶段与门

- **A 证据先行(无 schema 改动)**:轨迹回放检查器 + 共解析矩阵;从 08-03 trace
  挖边、补 28 条、修 sortie;每条边引一行 trace 为据。
  门:拆掉回放检查器的任一断言,自测必红。

  > **进度(2026-08-04)。** 共解析矩阵**已完成**:14 张未注册的屏进了模型、
  > `recognition.sweep` 与 `anchor_subsets` 落地、第一份报告跑了出来(见四·1)。
  > 门已验:拆掉 sweep 或拆掉 anchor_subsets,`tests/task/test-script-owned-model.cpp`
  > 的「The matrix reports which pages resolve on a screen besides its own」分别转红。
  >
  > **轨迹那半改了做法,理由是量出来的**:trace 里**一个页名都没有**——35889 行
  > `run-trace.jsonl` 零命中,`run.resources_validated` 发的是
  > `"elements":[],"pages":[]`,verb 表里没有任何页解析动词。而且 08-03 那次运行读的是
  > 一份 86046 字节的 `page-model.toml`(`sha256:f4ae1b3e…`),今天的文件是 124530
  > 字节,两份 `.before-*` 备份都不是它,工程仓库 08-04 才 git init——**那次运行的模型
  > 已经不存在了**,精确回放无从谈起。
  >
  > 因此改为:先加一个 additive 的 `framework.page_resolved` 事件(trace 版本不动,
  > `framework.*` 本就是可信框架自述的通道),让**每一次运行自动成为底卡**;回放检查器
  > 建在这个事件上并以合成 trace 自测。08-03 那 85 步只能作为一次性的人工补边来源
  > (页转移只存在于 `frames/daily-log.txt` 这份任务自己写的散文日志里)。
  >
  > **事件已落地,检查器没写。** 说准这条边界:`framework.page_resolved` 已经在
  > `observe.resolve_page` 铸票据处发出、也已进 trace 校验器;回放检查器一行都还没有。
  > A 阶段这半的状态是"不再缺输入",不是"做完"。
  >
  > **检查器必须先排除 `umbra-flow check` 自己的轨迹。** 矩阵扫描走的是同一个
  > `observe.resolve_page`,所以它照发这个事件:实测一次 check 跑出 **193 条**(85 屏 ×
  > 87 页里解析成功的那些,涉及 69 个不同页名,单屏最多 6 条);不带 `--sweep-pages`
  > 也有 70 条。不排除的检查器会把一次矩阵扫描读成"一局运行没有任何投递,却连着解析出
  > 193 次页面、62 张屏上各站不止一页",于是每一条页转移都变成伪 finding。
  >
  > **判据已从任务名改成前端(2026-08-05,`Check` 前端落地)。** 本文原先的排除条件是
  > `run.started.taskName` 等于 `falsification-matrix`,并把专属前端记为"打算中的正解、
  > 尚未实施"。现在 `trace::FrontEnd` 有第三个值 `Check`,`runFrameworkRoutine` claim
  > 的是它,整条流每一行都盖 `"frontEnd":"check"`(uf-chaos 实跑:5437 行全是这个值,
  > summary 与改动前逐字段相同)。**检查器按 `frontEnd` 排除,不再看任务名。** 任务名
  > 只是一条命名约定,项目自己就能写出一个同名任务;前端是一个闭合枚举,由 host 盖在
  > 每一行上,脚本层没有任何原语够得着。
  >
  > 这个名字的判据是"这次运行不投递任何输入":check 的 action sink 每个动词都拒绝,
  > 将来的轨迹回放检查器同样什么都不投递,所以它也归 `Check`,不必再起第四个名字。
  > 代价是一个 generation 不能先跑 task 再跑 check(反之亦然),前端互斥会拒;仓库里
  > 没有这样的调用者,`umbra-flow check` 每次都新开 host 与 generation。
  >
  > **「~28 条边」是一个计数口径而不是一次测量**:随四个二元选择(`<no page resolved>`
  > 断不断链、自环算不算边、raise 步算不算占位、没投递的步能不能作为起点)在 27–42 之间
  > 变动,27 只在最严口径下复现。取最严口径——它对上本文的数字,而且自环在 schema 里
  > 根本没有拼法(`kind` 没有"停留")。
  >
  > 另:本文说错的那条边是 `sortie -(sortie_enter)-> deploy`,实跑确实走
  > `sortie → danger_variance`;但更坏的一条是
  > `extreme_intro -(overlay_confirm)-> rest_point`——`rest_point` 在那 85 步里
  > **一次都没解析过**,而九·2 已裁决它退休。两条都要改。
- **B schema l2-v2**:三形态 / 外观门 / 证据 3、4 / state 与 over / preview 边;
  parse-build-encode 往返;regress 新格。门:每个新机制一对红绿
  (加上必绿,拆掉守卫必红)。
- **C 例程层与模式机**:五·1-5.4;daily.luau 收缩改写;回放 08-03 trace 验证
  改写后行为等价。门:周期账目平(open = close + spent)是回归断言。
- **D uf-chaos 策略化**:battle 知识表(第二个角色的表由开发者供给或从日志
  牌名起草)、装备流新标注与两项真机测量、route 条件化。
  门:一局无人值守菜单到菜单,步日志逐条可对账。

依赖:A 不依赖任何人;B 依赖 A 的证据(边表、共解析报告);C 依赖 B;
D 依赖 C 加真机。

## 九、裁决记录(2026-08-04 开发者直答)

1. 装备兜底:**先看装备再定**——策略先围观各角色现有装备,空槽优先,都满才
   提炼。「提炼 vs 等級最高」之争就此关闭;落地依赖围观边(D 阶段),在那之前
   现行「给等級最高」只是临时行为,不是规范。
2. 页面手术:**draw_pick 拆两页、rest_point 退休**照做。dice_tap/dice_roll
   **不动**——追查确认它们不是一屏两态而是两块屏(擲骰页锚 `擲骰`,另一页锚
   `請點擊畫面查看骰子結果。`,动作集合也不同),判据本身说两页;本文原先把它
   列为合并候选是误判,就此更正。开发者如在真机见到两句提示同屏,可重开。
3. 模式分区:**六模式**(menu / route / battle / event / camp / settlement,
   选人并入 menu)。
4. 工程目录:**已 init**(uf-chaos `775ae2d`,72 文件;`.gitignore` 排除
   运行输出,`.gitattributes` 关掉换行改写以保规范字节)。
5. 牌表来源:**押后到 D 阶段**,到策略化那一步再定手写还是从日志起草。
6. **season 相关内容整体搁置**(2026-08-05 直答):27 个 `season_*` 页、它们的元素与
   模板、以及矩阵对它们报出的行,一律不动,等开发者给出重新标注的办法之后再谈。
   随之落定三件:
   - 四·1 报的 `season_event` 命中 85 屏中的 59 屏(`seasonevent_crest` 是常量而不是
     页面签名)**不再是待裁项**,留档等重标。机制已量清,记在
     [WORKLIST](../WORKLIST.md) 一·1。
   - 三对锚点子集里有两对(`dismiss_overlay` ⊆ `season_flash_cards` /
     `season_mental_intro`)一并搁置,但三·4 的 `catch_all` 裁决**不重开**:它成立的
     理由是 `dismiss_overlay` 的签名描述的是一类画面,与包含它的是哪两页无关;而第三对
     `event_node` ⊆ `event` 是非 season 的同形证据,`catch_all` 的下一个候选就在那里。
   - 覆盖缺口**不受影响**,而且是搁置之后的第一优先级:25 页从未被任何 `[[screen]]`
     声明为归属页(矩阵报的 18 页零解析是其中一部分),其中只有 `season_deck_viewer`、
     `season_team` 两页是赛季的,其余 23 页是日常那批。
