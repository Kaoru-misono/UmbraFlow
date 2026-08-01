# 标注模型重构 — 能力集合、持有关系、多形态

> 状态:**设计结论有效并已落地**——能力集合 `{identify, interact, read}`、引用侧的
> `Holding` 与 `exercised`、具名 `Appearance`(空列表 = 由页面定位)、由引用派生的页面
> 签名都在代码里;词汇是 element / appearance(权威表见 `CONTEXT.md`),schema 是
> `umbraflow-authoring/v4` / `umbraflow-annotations/v3` / `umbraflow-trace/v2`。
> **变的只是实现位置**:模型按[页面模型上移到脚本层](2026-07-31-script-owned-page-model.md)
> 上移第二层 Luau,所以本文里关于 C++ 类型、CLI 动词、schema 键名的具体安排都是过渡态。
>
> **裁决链(读序)**:本文 §二 + §四之二(模型语义)→
> [页面模型上移到脚本层](2026-07-31-script-owned-page-model.md)(归属与第一层能力面)
> → [三层系统与 Agent 操作者](2026-08-01-three-layers-and-agent-operator.md)(顶层形态)。

## 一、为什么现在改

三个问题都不是设想出来的,是 2026-07-31 标注 `chaos-super` 的 `home` 与 `sortie`
两页时撞上的。

### 1. 一块像素只能有一个用途

`ElementKind` 是三选一的枚举(`AnchorElement` / `InteractiveElement` /
`InfoElement`),编译成同样三值的 `AnnotationType`。所以「既用来认页、又可以点」的
元素必须画两遍:两个 id、两份模板、每个观察周期匹配两次、各付一次像素预算。

`home` 页就有现成的例子:`home_story_label`(锚点)与 `home_story`(可点目标)是
**同一行字**,矩形几乎重合,今天是两个元素。

### 2. 「共享」是布尔,说不出谁拥有谁

`Element::Spec` 上有 `bool shared`,注释写的是「作者意图这份像素在别的页面复用」。
它是意图,不是关系:它答不出「这个元素的家在哪一页」,于是也答不出**改它的矩形会
影响谁**。

汉堡菜单把这个缺口暴露得很直接。它在 `home` 上标了一次,`sortie` 上还有一个、后面
几乎每一页都有。当时要么每页重画(N 份要同步的真相),要么用 `shared` 标记意图然后
——没有然后:「一个元素放在 N 页,不复制 element,改模板一次到位」这件事只在 GUI 里
有(`EditPage::placeExisting` / `shareRegionOnPage`),而这个项目是**用 CLI/agent
标注的**,四个 `page` CLI 动词全都在画新像素。

「布尔说不出关系」还有比意图更硬的证据:`shareRegionOnPage` 只在放到第二页时**单向
写一次** `shared = true`,没有任何东西反过来重算或清除它,另有两个独立的手动设置口
(`setRegionShared`、CLI `--shared`)。所以 `page add-target ... --shared` 能造出一个
**只有一次放置却 `shared = true`** 的元素——旗标和关系当场矛盾,而且没有谁会发现。
这才是 §2.2 推论 1 要删掉它的理由。模型能说「被哪几页引用」,说不出「家在哪一页」;
`Owned` 补的是后者,`bool shared` 从来补不了。

### 3. 一个语义元素只能有一套像素

返回按钮在暗背景下是白色、亮背景下是黑色。语义是一个东西,像素是两套。今天只能建
两个元素,靠命名约定绑在一起,而脚本得自己知道该用哪一个——那是把宿主的判断推给了
脚本作者。

## 二、设计

### 2.1 能力是集合,而且每个能力带自己的数据

**不做位掩码。** 三种能力各自需要不同的配套数据,而位掩码只能表达「有没有」:

```text
Capabilities {
    optional<Identify>  identify    // 参与页面签名(required / forbidden)
    optional<Interact>  interact    // 授权与投递
    optional<Read>      read        // OCR 参数:单行还是整块、字符集
}
```

**不变量:至少一个能力存在。** 三个都空的元素没有任何东西能到达它,它不是一个
「暂时没用」的元素,而是一个无法解释的条目。构造时拒绝。

把配置放进各自的能力里,而不是平铺在元素上,买到的是一条结构性事实:**不具备读取
能力的元素无法携带 OCR 参数**。平铺的话它只能靠纪律。

**立刻的收益**:同一块像素同时认页和可点时,一个周期只匹配一次。今天是两次。

**`Read` 不带模板(2026-07-31 开发者裁定)。** 运行时只按矩形取像素交给 OCR,不做
模板匹配。理由是模板会锁死当前值:`sortie_level` 存的模板是 `Lv.65`,而 65 正是要
读出来的东西,匹配它等于要求「等级不变」才能读出等级。代价是这个矩形**没有定位
能力**,布局若移动它就读错地方——接受,因为这个项目的目标是固定分辨率固定 DPI 的
单一窗口,布局不移动是已经依赖的前提(`ProjectFingerprint` 就是这条前提的守卫)。

> **更正(2026-07-31):裁决对,但守卫认错了。** 模板锁死当前值那段推理无懈可击,
> 不动。但 `ProjectFingerprint` 只有四个数——`{width, height, dpiX, dpiY}`
> (`resource.hpp:86-91`),它守的是**窗口几何**,不是窗口**内部布局**。滚动位置、
> 一个变宽的名字把数字挤走、列表少一行,矩形都会移动而 fingerprint 全程不变。
>
> 这件事在 `Read` 上比在别处严重,因为 **`Read` 是整个设计里唯一 fail-open 的能力**:
> 模板没匹配上是 `nil`,读错位置的 OCR 返回一个**像模像样的数字**,没有置信度可给。
> 两条便宜的修法,都服务已经存在的需求,不是为想象需求造机械:
>
> 1. 把前提改述成**每个元素自己的作者声明**——「这一格的矩形是布局稳定的」,而不是
>    一句由 fingerprint 守卫的项目级前提(它守不了)。
> 2. 读取的 trace 行必须带**矩形与解出的文本**。别的能力靠分数说话,读取没有分数,
>    没有这两样就无法在证据流里定位一次读错。
>
> 顺带:P1 已经预留了 `BaseToLiveTransform` + 确定性重采样,分辨率自适应是要拉进
> P1 的——固定分辨率这个前提**在第二个游戏之前就会松动**,不是到 P3 才松。

### 2.2 拥有 / 引用放在页面这一侧

元素本来就是项目级的,页面通过 placement 引用它。所以持有关系是**页面记录**的属性,
不是元素的:

```text
PageReference {
    elementId
    holding: Owned | Referenced   // 新增
    exercised: Capabilities       // 新增:这一页实际行使它的哪几种能力
    searchRoi?                    // 本页可细化
}
```

两层的分工:**元素声明它能做什么,引用声明这一页用它做什么**,后者是前者的子集。

> **更正(2026-07-31,对照 `modules/annotation/source/annotation/authoring-document.hpp`)。**
> 本节描述的页面侧引用**已经落地**,由
> [page-centric authoring](../archive/plans/2026-07-26-page-centric-authoring.md) 的 Phase 2 完成:
> 授权文档 schema 已经是 `umbraflow-authoring/v2`,元素是项目级的,放置是页面侧的,
> 今天的 `AuthoringPlacement` 就是 `{ pageId, elementId, searchRoi }`。那份计划的
> §2 第 3 条写的「Sharing becomes what it claims to be: one element, N placements」
> 和本节是同一句话。
>
> 所以本节真正新增的只有 `holding` 和 `exercised` 两个字段。这不削弱本节 ——
> 恰恰相反,最贵的那次搬迁(把 membership 从 element 挪到页面侧)已经付过了,
> 剩下的是在既有 `AuthoringPlacement` 上加两个字段。

三条推论:

1. **`bool shared` 删除。** 被两个以上页面引用的元素**就是**共享的,不需要第二个
   事实去声明。而「第二个页面出现之前意图也要能记录」这条顾虑由 `Owned` 承担:它是
   作者声明「这个元素只属于这一页」,工具据此在别处引用时拒绝。

   > 更正(2026-07-31):这是一次**反转**,不是收尾。
   > [page-centric authoring](../archive/plans/2026-07-26-page-centric-authoring.md) §2 第 5 条当时
   > 明确裁定「The authoring-only `shared` flag **stays what it is today** — author
   > intent that an element is offered for reuse — carried on the element」。本条推翻
   > 它。推翻是对的(理由见 §一.2 更正:旗标能和关系当场矛盾),但要认账这是改判,
   > 别当成前一次没做完的尾巴。
   >
   > 迁移上有一处后果值得先记下:`shared` 今天是 workbench「Shared regions」面板的
   > 过滤条件(`panels.cpp:1514-1530`),而那个面板正是拖到第二页、通向
   > `placeExisting` 的入口。删字段就要把面板改成按 `Owned` 反过来筛,否则入口一起
   > 没了。
   >
   > > **这条已作废(2026-07-31,`b57b67b` + `f768e6c`)。** 面板连同整个 GUI 都归档了,
   > > 所以没有面板要改。它守的那个入口改由 CLI 提供:`umbra-authoring page reference`
   > > 就是「把已有元素放到第二页」的动词。本条保留,是因为它记下的
   > > 那个顾虑——「删 `shared` 会连带删掉通往第二次放置的唯一入口」——正是 §四之二.1
   > > 把 `placeExisting` 列为硬前置的理由。
2. **这是编辑护栏,这才是它真正的价值。** 改一个 `Owned` 元素的矩形是安全的;改一个
   被三页引用的元素,三页都会变,工具必须当场说出来。

   > 补充(2026-07-31):原句「今天的**模型**说不出这句话」字面上成立——
   > `modules/annotation` 里确实没有这个查询。但 workbench 侧已经有了:
   > `pagesPlacedOn(draft, id)`(四个面板在用)和改模板时的 `RetemplatedRegion
   > ::otherPlacements`。
   >
   > 所以本条的价值要说准:不是「从没有到有」,而是**把前端各自实现的一个查询,
   > 变成关系本身的读法**——于是每个编辑动作、每个前端自动拿到同一句警告。缺口在
   > 覆盖面:改颜色键、改阈值今天都没有这句警告(改本页 `searchRoi` 不需要,它按
   > 构造只动这一页的 placement),授权 CLI 则一句都没有。
3. **`allowed_page_ids` 整个删除。** 一个页面引用了某元素并且行使 `interact`,
   **就是**授权。授权列表和引用关系是同一件事的两种写法。

   > 补充(2026-07-31):**原文的理由成立,但要说清它成立在哪一层。** 授权文档里这个
   > 列表已经没有了,写的是 `[[placement]]`;`catalog()` 那份是构造时由 placements
   > 反演的读模型,相等由构造保证。
   >
   > 真正对得上原文那句的是**第三处**:运行时清单被**独立解析**——
   > `runtime-manifest.cpp` 直接读 `allowed_page_ids`,授权检查(`authorization.cpp`)
   > 读的就是这份解析结果,运行时从头到尾看不见 placements。所以「留两份就要保持
   > 相等,而没有任何东西检查它们相等」**在运行时清单这个信任边界上完全成立**:
   > 只有走编译器那条路才相等,清单自己不携带这个证明。
   >
   > 顺带纠正一个想当然:不能说「每个 `allowed_page_ids` 都是单元素列表」。
   > `authoring-compiler.cpp:693-698` 里放置为空时列表就是**空**的,而锚点永远为空
   > ——`catalog.cpp:441-450` 明确要求锚点的列表必须为空(「page_anchor membership
   > must be expressed by page signatures」)。长度 0 是**规定的编码**,不是多余状态。
   > (磁盘上 `chaos-super` 的清单里 15 条 `allowed_page_ids` 确实全是单元素,那是
   > 因为空列表根本不写出来。)
   >
   > 还有一段必须同一次改动一起处理:`authoring-document.cpp:403-409` 那条
   > `PERMANENT BRIDGE -- do not "clean this up"` 注释,把
   > `umbraflow-annotations/v1` 称作 **FROZEN**,并说明这个反演之所以必须留在原地,
   > 正是因为运行时契约冻着。schema 升版解冻了它,本条又删掉了它要产出的东西——所以
   > 那段注释必须跟着走,否则下一个维护者会把它读成否决票。**已随升版一起删除。**

> **实现后果(2026-07-31):`cycle_find` 从此要求该周期已解析出页面。** 这不是选择,
> 是被逼出来的——每页的细化 ROI、钉死的形态、以及 interact 那条边都挂在引用行上,
> 所以 `evaluateActionTarget` 必须先知道是哪一页,不存在无页入口。这与 §四之二.3
> 「`appearances` 为空 ⟹ 页面认出来了矩形就在标注的地方」是同一句话的两面。
>
> **一个脚本可见的变化,需要裁决:** `modules/task/runtime/ctx.luau` 的
> `ctx:cycle(fn)` 不解析页面,于是脚本在里面直接 `cycle:find(...)` 而没先
> `cycle:page()`,现在会得到 `action_rejected`。`ctx:wait_for_page` 不受影响
> (`observeCycle` 总是先解析)。让 `view:find` 自动解析页面是**策略变更**,不是修复,
> 所以实现时没有动它。要么改 `ctx.luau`,要么把「find 之前先 page」写进脚本约定——
> 这条留给开发者。
>
> > **已裁决并落地(2026-07-31):写进脚本约定,不自动解析;错误种类换成
> > `page_unresolved`。** 上面那句「会得到 `action_rejected`」按此读作
> > `page_unresolved`。不让 `view:find` 自动解析,是因为自动解析的两个缺点是结构性的:
> > 它**藏起成本**(页面解析是一个周期里最贵的部分),而且**失败时更难懂**(解析不出
> > 页面会以「find 失败」的形式冒出来)。新种类 `AutomationErrorKind::PageUnresolved`
> > (wire `page_unresolved`,`FailureResponse::StepFailed`)与 `ActionRejected` 分开,
> > 是因为两种失败的修法相反——前者要改**脚本的调用顺序**,后者要改**标注**;response
> > 不是 `Retry`,是因为页面解析到**周期**上,同一个周期重复调用永远不会成功。约定写在
> > `modules/task/runtime/ctx.luau` 的 `view:find` / `ctx:cycle` / `ctx:cycle_find` 三处。
>
> 另一条诚实标注:`CycleLedger::consume` 里那个「没有页面」的分支从任务面已经
> **不可达**了(拿不到 hit 就无从点击)。它作为 fail-closed 守卫保留。
>
> > **更正(2026-07-31,实现时):「没有任何东西能让它变红」这半句不成立了。**
> > 原文据此把它归进「守着但测不出」那一类。实际上**脚本**做不到而**C++ 做得到**:
> > `tests/task/test-task-binding.cpp` 用第一个周期取到 hit,再把它带到第二个未解析
> > 页面的周期上,就打进了这个分支;改掉它的种类该用例当场变红。不可达的是任务面,
> > 不是测试面。

> 与 [三层 Task System](2026-07-29-three-layer-task-system.md) §4 的衔接:
> `cycle_click` 不收 page 参数、四要件「从校验变成构造上不可能违反」这条性质不受
> 影响。授权证据仍然是宿主用该周期解析出的页面构造的;变的只是宿主查表时读的是
> 引用关系而不是一个 id 列表。

### 2.3 多形态:一个元素,多个模板,声明顺序

```text
Element {
    id, name, capabilities, searchRoi
    appearances: [ Appearance{ sourceId, templateRect, colourKey, threshold } ]   // 有序
}
```

脚本只认识 `uf.elements.back` 一个名字,授权也只有一个 id。两点都比「两个元素靠
命名约定绑定」干净,而且把「该用哪一套像素」这个判断留在了宿主。

**三条必须钉死,否则这个设计会变成灾难:**

1. **顺序是声明顺序,不能按「上次命中的排前面」。** 那样性能更好,但它让行为依赖
   历史。§10 的确定性要求是同 seed 同观察序列可复现,自适应排序直接违反它。

   > 更正(2026-07-31):**禁自适应是对的,但「声明顺序」应当只做平局裁决,不做选择
   > 规则。** 「先过阈值者胜」同样是确定性的——所以确定性不是反对它的理由;真正的
   > 理由是它会**移动点击位置**:`resolveClickPixel`
   > (`recognition-runtime.cpp:637-670`)是从 `matchedRect` 推点击点的,一个过宽的
   > 早序 appearance 抢答之后,返回的是**它自己**的矩形,点击就落错像素,而下游没有任何
   > 东西会察觉。
   >
   > 改述:**声明顺序在归一化裕度打平时裁决,不决定谁匹配。** 代价为零——代价那行
   > 本来就承诺了要搜完 N 个。
   >
   > 但比较必须小心:**appearance 之间的分数不可直接比较**。`maximumSad` 是
   > `templateWidth·templateHeight` 与阈值的函数(`catalog.cpp:300-339`),而每个
   > appearance 各有自己的 `templateRect` 和 `threshold`。唯一精确的整数比较是交叉相乘:
   > `scoreᵢ · maxSadⱼ` 对 `scoreⱼ · maxSadᵢ`。
   >
   > **补第 4 条:appearance 集合内部的预算中断。** 任何 appearance 搜索途中撞到预算或
   > 截止,就是整个元素停止——**不能**「取已搜过的里面最好的那个」。后者会让答案变成
   > 比较预算(一个配置值)的函数,那才是真正违反 §10 的那种违反。
2. **命中的是哪个 appearance 必须进 trace。** 否则「为什么这次匹配上了」在证据流里
   答不出来。engine 的 `engine.action_found` / `engine.page_resolved` 要带 appearance
   身份。

   > 更正(2026-07-31):事件名带 `engine.` 前缀(`trace/event.cpp:79-80`)。更要紧
   > 的是,这一条撞上一个**有意为之**的既有决定:`Page::Score` 只携带最差的那个必需
   > 锚点,`trace/event.hpp:187-201` 明确写了这是刻意的(「a trace line is read rather
   > than queried」)。要给每个锚点都带上 appearance 身份,就得推翻那条决定。代价最小
   > 而且大概率正确的做法是只加 `Score.worstAnchorAppearance`——已解析页面上最差的那个
   > 必需锚点本来就是最接近翻车的那一个。上游 `AnchorEvidence`
   > (`recognition.hpp:68-77`)也还没有 appearance 成员,那是要先加的字段。
3. **每个 appearance 各自过证伪,集合整体也要过。** 这是最大的风险:多试几个模板直到
   有一个命中,本质上是在放宽判定。要求是「每个 appearance 命中它自己那个状态」**并且**
   「整个集合在该拒绝的状态上全部落空」。否则它就是
   [colour-key-annotation](../pitfalls/colour-key-annotation.md) 里那个
   「小掩码恒命中」陷阱的高阶版本——看起来最可靠,实际什么都没区分。

   > **更正与细化(2026-07-31)。** 三件事。
   >
   > **(a) 引的那段 pitfall 机制已经过时。** `colour-key-annotation.md` 的「根本原因」
   > 说阈值「对掩码一无所知」,并算 `920·255·0.01`。但 `normalizedScore`
   > (`vision/sad.cpp:47-62`,commit `c392161`,早于该 pitfall 的 `c26fbbf`)会把
   > 加权和按 `templatePixels / totalWeight` 缩回整模板尺度,所以实际判据是
   > **「掩码内的加权平均灰度误差 ≤ 255·(1−t)」**——阈值是掩码相对的。观察到的
   > 失败是真的,但成因是另一个:27 个饱和白像素**总能在大 ROI 里找到一个全对齐的
   > 位置**(`page-modeling-and-multi-step.md:246-254` 写的才是对的那版)。
   > **实施前先修这份 pitfall**,否则缓解措施会瞄准阈值公式(它本来就是对的),
   > 而不是瞄准掩码大小 × ROI 大小(真正的驱动因素)。
   >
   > **(b) 「集合整体也要过」在拒绝侧不需要新的运行时算子**——
   > `¬(∃i hit_i) ≡ ∀i ¬hit_i`,集合拒绝就是逐 appearance 拒绝的合取。它需要的是
   > **完备性**:每个 appearance 对**每个**拒绝状态都测,而不是各测各自剪出来的那些。
   > 作者的自然习惯(暗色的只在暗色屏上测)恰好跳过了要紧的那一半。
   >
   > **(c) 真正危险的状态不在拒绝集里,是别的 appearance 的命中状态。** `S_light` 上
   > `back` 确实存在,它不属于任何人的拒绝集;若 `on_dark` 在那里刚好压线通过,
   > 逐 appearance 矩阵**全绿**而元素已经坏了(点击落在 `on_dark` 的矩形上)。
   >
   > 所以矩阵要求写成可实现的四条(元素 `back`,appearance `v₀=on_dark`/`v₁=on_light`,
   > 屏幕 `S_dark`/`S_light`/`S_none`,`S_none` 必须是**真的**无该 UI 的画面):
   >
   > - **P1 完备性**:对每个 `(v, s)`,实测 `hit == owns(v, s)`,N×M 全测,
   >   **包含非对角格**。
   > - **P2 集合拒绝**:无人拥有的状态上,**折叠后**的元素报 miss(逻辑上由 P1 蕴含,
   >   但要单独断言在折叠结果上,才能抓住折叠实现的 bug)。
   > - **P3 归属**:被 `v*` 拥有的状态上,折叠结果的 `appearanceId == v*` **且**
   >   `matchedRect == v*` 的矩形。这条正是「先过阈值者胜」会违反的那条。
   > - **P4 分离度**(整数、精确):对每个 `v ≠ v*`,
   >   `score_{v*}(s) · maxSad_v · k <= score_v(s) · maxSad_{v*}`。`k` 取仓库里已测得的
   >   跨屏 miss 落差 2.85×–4.15×,`k = 4` 站得住。
   > - **P0 构造期前置** —— ~~掩码全选像素 ≥ ~50,在 `Appearance::create` 里**拒绝**~~
   >   **已推翻(2026-07-31 开发者裁决),降级为绘制期警告。** `Appearance::Spec` 携带的
   >   是 `sourceId` 不是像素,那一层数不了掩码;更要紧的是**像素数本身就是错的度量**
   >   ——实测 27 像素白掩码太小测不到东西,而 `繼續進行` 键的橙底 14112 像素(占 77.2%)
   >   太均匀,同样分不出东西,一条「≥50」的下限会放过后者。落地形状:`page create` /
   >   `page add` 在全选像素 < 50 **或**占矩形 ≥ 50% 时附 `warning`,`ok` 仍为 true,元素
   >   照常写盘;**闸门是 `umbra-authoring check` 的证伪矩阵**,那是实测而不是猜。将来若
   >   要硬闸门,它该量**结构**而不是**计数**。
   >
   > **红证**(项目纪律:测试只有在去掉性质后会变红才算数):
   > R1 把那个 27 像素病态掩码当 `v₀`,断言检查器在 `(v₀, S_light)` 报 misfire——
   > 删掉 P1 非对角格它就再也红不了。**R2 最值钱**:把过宽的 `v₀` 声明在**前**,
   > 匹配 `S_light`,断言返回 `v₁` 和 `v₁` 的矩形——「先过阈值者胜」下它是红的,
   > 这一条才让「声明顺序只是平局裁决」从一句话变成事实。R3 造一个赢但裕度 `< k` 的
   > 样例,断言证伪器**拒绝该模型**(否则 `k` 会在调参中漂到 1)。R4 随 P0 一起改判:
   > 断言的是绘制期**警告**而不是构造期报错,红证在
   > `tests/authoring/test-authoring-cli.cpp` 与 `entry/authoring/command-runner.cpp` 的
   > `maskWarning`。
   >
   > **落点**:`ModelCheckCell`(`preview.hpp:278-303`)已经是「元素 × 屏幕」的证伪
   > 矩阵,`classifyModelCell` 已经会把「不该命中却命中」标成 `Misfire`——**它就是这个
   > 矩阵,只差一个 appearance 维度**。不加这一维,V 个 appearance 会塌成一个 Hit/Miss,
   > P1 的非对角格在结构上就无法表示。

**代价**:N 个 appearance 就是 N 次搜索,像素预算按最坏情况算,不是按平均。

> **更正(2026-07-31,见 §四之二.4):这个代价的适用范围小得多。** 当「哪个形态适用」
> 由**页面**决定(返回键此页白、彼页黑),appearance 在 `PageReference` 上钉死,那一页
> 只搜一次。需要搜 N 次的只剩「同页内、由运行时状态决定」那一类(加速按钮 1x/2x/3x)。
> 证伪的压力也集中到同一小类:**同页内互斥的那几个形态必须两两落空**。

### 2.4 三者合起来

```text
Element "back"
  capabilities: { identify, interact }
  appearances: [ on_dark(白字, key=白), on_light(黑字, key=黑) ]

Page "sortie"  -> PageReference{ back, Referenced, exercised = { interact } }
Page "battle"  -> PageReference{ back, Referenced, exercised = { interact, identify } }
```

`battle` 额外拿它当签名的一部分,`sortie` 只用来点。同一份像素、同一个 id、两种
用法——这是今天的模型表达不了的那句话。

> **更正(2026-07-31):这个例子今天的编译器直接表达不出来,不只是「模型说不出」。**
> `authoring-compiler.cpp:685-709`:放置数 ≤1 时运行时 element **保留元素自己的
> id**;≥2 时(`:712-727`)展开成 N 个**派生 id**
> (`derivedRuntimeRecognizerId(element.id(), placement.pageId)`)。而这个例子里
> `back` 放在两页上,于是元素 id 下**根本没有 element**——`battle` 的签名会引用
> 一个运行时目录里不存在的 id。
>
> 更深一层:页面签名是**严格合取**——`PageResolver::resolve` 对 `page.required()` 做
> `candidate = candidate && p_evidence->hit()`(`recognition.cpp:391-397`)。所以
> **appearance 的折叠也不能靠编译器展开**:把一个锚点的 V 个 appearance 编译成 V 个必需
> 锚点,会把「任一 appearance 命中」变成「全部 appearance 都要命中」。折叠必须发生在
> **单个 `AnchorEvidence` 内部**,在证据到达 resolver 之前。
>
> 连带两处:`RuntimeManifest::findAsset(elementId)` 变成一对多;创建期的几何校验
> (`recognition-runtime.cpp:272-296`)要逐 appearance 做。
>
> 还有一条把两件事连起来的裁决,建议在本节直接钉死:**行使 `identify` 的引用不得
> 细化 `searchRoi`**(锚点走元素级 ROI)。否则同一块像素在认页和点击两条路径上用
> 不同 ROI 搜两次,恰好把「一个周期只匹配一次」这个收益吃掉。这条同时把 §五.2 收窄成
> 「只对不行使 identify 的引用暴露细化入口」。

## 三、迁移(已废止)

> 本节那份「两个 schema 一起升版」的迁移计划已作废。实际落地的是
> `umbraflow-authoring/v4` 与 `umbraflow-annotations/v3`(外加 `umbraflow-trace/v2`),
> 与本节写的 v3/v2 顺序都不同;此后模型的实现位置又按
> [页面模型上移到脚本层](2026-07-31-script-owned-page-model.md)上移第二层,项目文件格式
> 改由第二层定。经过在 git 历史里。

## 四、已落地的相关改动(已废止)

> 那两条为了不阻塞当晚标注而加的临时补丁都已被落地的能力模型收回:`page add-info` 并进
> `page add --capability read`,`--shared` 与它设置的 `bool shared` 由引用侧的 `Holding`
> 取代。

## 四之二、2026-07-31 追加裁决(开发者,review 之后)

### 1. workbench GUI 弃用(已于 `b57b67b` 执行)

爆炸半径因此缩小(`entry/workbench` 是 `AnnotationType` 与 `shared` 最大的消费者),
但有三样东西当时**只存在于 GUI 里**,弃用等于它们消失,必须在 CLI 侧补上——三条都已
补上:`page reference`、按页 `searchRoi`,以及证伪矩阵的 CLI 动词
`umbra-authoring check`(`41e0816`)。

| 只在 GUI | 后果 |
|---|---|
| `placeExisting` / `shareRegionOnPage` | `Referenced` 将无法产生,`holding` 失去意义。**硬前置** |
| `InteractiveRegion::setSearchRoi` | `PageReference.searchRoi?` 没有作者入口 |
| `ModelCheckCell` / `classifyModelCell` | **证伪矩阵消失** |

第三条是新的前置条件。§2.3 把「每个 appearance 各自过证伪、集合整体也要过」定成多形态
能否成立的前提,而那个矩阵是 workbench 代码(`entry/workbench/preview.*`)。GUI 一
弃用,前提就没有工具支撑。而且矩阵结论本来就没进项目文件——`chaos-super/annotations.toml`
只有 2 条 regression 且都是 positive。所以:**把证伪矩阵搬进 CLI,并把结论落进项目
文件**,从改进升级为前置条件。

### 2. 新增一种交互形态:只要交互范围,不要标定内容

页面被认出之后,「哪些区域能点」就已知;具体点哪里由 OCR 决定,那块像素不必标定。

**不新增第四种能力**,把这条性质提到 `appearances` 上,因为它和已裁定的「`Read` 不带
模板」是同一件事:

```text
Element {
    id, name, capabilities, searchRoi
    appearances: [ ... ]   // 空 = 这个矩形由「页面被认出」定位,不由自己的像素定位
}
```

- `appearances` 非空 → 元素能自定位,可以行使 `identify`
- `appearances` 为空 → 只能靠页面定位;构造时应拒绝 `identify`(它没有像素可当证据)

一条规则同时覆盖 `Read` 和交互区域,不必为后者发明新概念。代价与 `Read` 已接受的
那条相同:布局若移动,矩形就指错地方。

> **更正(2026-08-01)**:上面第二条放宽。没有像素不等于没有证据——区域**读出的文字**
> 也是证据。现在的规则是:`appearances` 为空的元素要行使 `identify`,必须声明 `read`,
> 并且有期待文字可核;两样都没有仍然拒绝,措辞照旧。检查分两处,分法本身是设计:元素侧
> 只回答「这个元素能不能当证据」(`Element.new` 看有没有 `read`),「这一页上它该读出
> 什么」归页面(`Page.new` 看这一行有没有期待文字)——共享的标题栏每页读出的字不一样,
> 只有页面知道。期待文字因此也可以写在引用侧,优先于元素侧。见
> [三层系统与 Agent 操作者](2026-08-01-three-layers-and-agent-operator.md) §九-8。

### 3. ~~点击不再要求命中已标注元素~~ —— 已撤销

> **撤销(2026-07-31,同日晚些时候)。** 本条初稿裁定「driver 获得任意区域点击能力,
> 约束下放到 Luau」。澄清实际场景之后,这个让步**不需要发生**,四要件一条不掉。
> 保留标题是为了让读到旧引用的人找得到这里。

驱动这条初稿的场景是:页面认出来之后,由 OCR 决定点哪里,而被点的东西没有单独标注。
澄清后的真实模型不是「一个区域里有多个可点子元素」,而是:

> **N 个各自独立的元素,每个都同时具备 `read` 和 `interact`。** 每一块本身就是要点的
> 目标;OCR 只是告诉脚本**该点哪一块**。

于是流程完全落在既有动词上:

```lua
-- 页面认出来了,每个 slot 的矩形都已知(appearances 为空 ⟹ 由页面定位)
for _, slot in ipairs(candidates) do
    local reading = uf.cycle_read(ticket, slot)          -- 唯一新增的动词
    if reading and reading.text == want then
        uf.cycle_click(ticket, uf.cycle_find(ticket, slot))   -- 两个都是现有的
    end
end
```

**没有裸坐标,没有新的投递路径,`cycle_click` 一字不改。** 点的是一个标注过的元素,
授权仍然是「这一页引用它且行使 `interact`」。四要件全部保持「构造上不可能违反」,
[三层 Task System](2026-07-29-three-layer-task-system.md) §4 那句话不需要更正。

对 `appearances` 为空的元素,`cycle_find` 的语义是「页面认出来了 ⟹ 矩形就在标注的
地方」,必然成功;它不做像素匹配,因为没有模板可匹配。这正是 §2.1 为 `Read` 已经
接受过的那条代价,原样适用。

**由此还得到一条边界**:文字**不参与定位**,只在若干**已授权**目标之间做选择。所以
「怎么算匹配」(全角半角、繁简、包含还是相等)是**纯策略,归 Luau**——比错了也只是
点了另一个同样被授权的目标。宿主不拥有这条规则,`cycle_read` 因此**不收** `expected`
参数。

> **补充(2026-08-01)**:**期待文字本身可以作为数据写进项目文件**——「这一格是出击」
> 是关于 UI 的事实,写下来才可证伪、可重放。这与本条并立,不冲突:数据住在标注里,
> 核对逻辑仍归 Luau,`cycle_read` 仍然不收 `expected`。见
> [三层系统与 Agent 操作者](2026-08-01-three-layers-and-agent-operator.md) §五-4。

### 4. 三种「一个元素多种形态」,是三件不同的事

开发者给出三个实例,它们不能共用一套机械:

| 实例 | 性质 | 表示 | 每周期搜索 |
|---|---|---|---|
| 加速按钮 1x / 2x / 3x | **状态读出**——命中哪个本身就是信息 | 一个元素,**有名字的** appearance 列表;命中身份上到脚本面 | N |
| 返回键此页白、彼页黑 | **页面决定**——标注期就知道哪套适用 | 一个元素,两个 appearance,**在引用侧钉死** | 1 |
| 卡片点击后向上扩展 | **几何变化**——位置和尺寸变了 | **不进模型** | 0 |

**(a) 状态读出:appearance 要有名字,而且要上到脚本面。**

```text
Appearance { name, sourceId, templateRect, colourKey, threshold }

hit.appearance == uf.appearances.speed_2x   -- 脚本据此知道当前倍速
```

§2.3 原本只要求 appearance 身份进 trace;这个实例说明它还要到达**脚本**。

**(b) 页面决定:钉死在引用侧,不要靠试。**

```text
PageReference {
    elementId
    holding, exercised, searchRoi?
    appearance?          // 这一页上,它长这个样子
}
```

> **更正(2026-07-31,实现时发现):钉死只对 `interact` 成立,对 `identify` 不成立。**
> 锚点扫描是**全局的**——`pageAnchorOrder` 是跨页去重的并集,每帧走一遍,而且它跑在
> **页面被确定之前**(那正是「一次搜索服务所有页面」的由来)。所以扫描时根本不知道
> 该用哪一页的钉子,`identify` 只能跨所有 appearance 折叠。引用侧的 `appearance` 在
> `identify` 上被接受但忽略,并在声明处写明。
>
> 下面这句话因此只对 `interact` 成立:

**这一条把 §2.3 最大的风险直接消掉。** 「多试几个模板直到一个命中」危险,是因为它
本质上在放宽判定;页面钉死之后那一页**只搜一次**,没有任何放宽,像素预算回到 1。
于是**需要搜 N 次的情形只剩 (a) 一类**——同页内、由运行时状态决定的形态。§2.3 那条
「N 个 appearance 就是 N 次搜索」的代价只落在这一小类上,不是所有多形态元素。

证伪的压力也随之集中:**同页内互斥的那几个形态必须两两落空**。三个倍速图标很可能
只差一个数字,掩码选大一点三个就互相命中而矩阵全绿——正是 §2.3 里 P1 非对角格要抓
的那一类。

**(c) 几何变化:不进模型。** 卡片向上扩展,原来的位置仍是卡片的一部分,所以**点**用
收起态矩形就够。要**判断**是否展开,则回到状态判别——用 `cycle_read` 看那块区域有没有
冒出别的字。**不能靠几何判别。** 而「必须先点 A,B 才存在」是顺序依赖,属于策略,归
Luau;模型不编码它,脚本没展开就去点 B,点到的是一个**已授权区域内**的空白,那是脚本
bug,不是安全问题。

**appearance 的定义边界**(写死,否则它会变成什么都往里塞的口袋):

> **appearance 换的是同一块像素长什么样;它不换这块像素在哪,也不换它是什么东西。**
>
> - 位置或尺寸随运行时变 → **不是** appearance
> - 「命中哪个」本身有意义 → 是 appearance,要有名字并暴露给脚本(a)
> - 「哪个适用」由页面决定 → 不进有序列表,在引用侧钉死(b)

这条边界同时**关闭 §五 开放问题 1**:「暗色可点、亮色是禁用态」是**状态**,按 (a)
处理——有名字的 appearance,脚本读命中身份自己决定点不点。appearance 不需要携带能力差异。

> **确认(2026-08-01)**:状态读出沿用本条 (a) 的**有名字的 appearance** 机制;同日一度
> 提过的替代方案——把互斥状态拆成各自独立的元素(`speed_1x` / `speed_2x` / `speed_3x`
> 各成一个元素)——**已否决**。见
> [三层系统与 Agent 操作者](2026-08-01-three-layers-and-agent-operator.md) §六。

### 5. `RecognizerId` 改名 `ElementId`

开发者裁定改名。`RecognizerId` 来自 2026-07-26 之前的模型,那时「recognizer」确实是
主名词。在那一版目标模型里 recognizer 是编译器**生成**的、每 (元素, 页面) 一个的运行时
产物(`derivedRuntimeRecognizerId`),而一个只有 `read` 能力的元素什么都不识别。schema
本来就要破坏性升版,现在改是免费的;放过这次,下次要改就得再破一次版。

> 更正(2026-07-31,词汇统一):这里保留的「recognizer 指编译产物」这条分工也已经作废。
> 同一天的第二次改名把这个词整个撤掉了——编译产物叫 `CompiledElement`,`uf.recognizers`
> 叫 `uf.elements`,trace 的 `recognizerId` 叫 `elementId`。recognition 只留在它指
> 「识别这个动作」的地方(`RecognitionCatalog`、`RecognitionRuntime`)。见 CONTEXT.md
> 「Annotation model」一节。

波及 `Element::Spec.id`、`AuthoringPlacement.elementId`、`PageSignature` 的两个向量、
`CompiledElementSpec/CompiledElement`,以及 TOML 的字段名。

### 6. 一个页面的多种形态:判据是授权集

页面身份是**保证**——`cycle_click` 要求 ticket 已解析出页面,授权就是「这一页引用了
该元素且行使 `interact`」。所以形态怎么归类由授权集决定:

> **形态改变了「这一页允许点什么」→ 它是另一个页面**(另一个 id、另一份签名)。
> **形态只改变「现在该点哪一个」→ 同一个页面的状态,由脚本判别。**

后者**不需要任何新机械**,复用 §四之二.4 已定的三种手段:`cycle_find` 返回 hit/nil
(某元素只在形态 A 出现)、`hit.appearance`(同一位置多种外观)、`cycle_read` 的
text/confidence(没有稳定模板时)。页面保持一个 id,签名由**两种形态都成立的不变
标记**组成。

**已知边界,写明而不是留给实现去补:**

卡片向上扩展会**覆盖邻居槽位的矩形**。宿主的授权是**按矩形给的**——矩形没动,像素
动了。于是在展开态点邻居,宿主会放行,而点到的是展开的卡片。宿主拦不住,因为邻居的
矩形确实在这一页的授权列表里。

更硬的一句:**`appearances` 为空的元素,点击前无法做像素级复验。** 有模板的元素,
`cycle_find` 在同一周期刚匹配过,那本身就是一次复验;没有模板的元素只有「页面认出来
了」这一句,没有任何东西能证明那块像素此刻还属于它。这是 §2.1 为 `Read` 接受的那条
代价在 `interact` 上的翻版,而且后果更重——读错只是读错,点错是**投递了一次动作**。

缓解,按代价排:(1) 点之前 `cycle_read` 确认那块读出来的还是预期内容——用已定的东西,
零新机械,**建议作为默认写法**;(2) 给槽位加一个可匹配的小特征(边框、角标),让它
变成有模板的元素,`cycle_find` 即复验;(3) 把展开态定成另一个页面——只有当展开确实
改变了授权集才值得。

### 7. `cycle_read` 的形状、trace 与预算

```text
cycle_read(ticket, region) -> reading | nil

reading { text, confidence, rect, region_id }
```

**confidence 是必需的,不是锦上添花。** `cycle_read` 身兼两职——选目标、判状态。判
状态时失败方向不对称:展开态读不出字 → 脚本以为没展开 → 继续等(**安全**);收起态
凭空读出字 → 脚本以为展开了 → 点错东西(**危险**)。没有 confidence,脚本区分不了
「真的有字」和「读岔了」。这也正好补上 §2.1 更正指出的那个洞:`Read` 是设计里唯一
fail-open 的能力。PP-OCRv6 本来就逐行给置信度,取出来几乎不花钱。

**trace 要记的字段**(读取路径没有分数可当证据,所以必须靠这些顶上):

| 字段 | 为什么 |
|---|---|
| `region_id` + 矩形 | 读的是哪一块;矩形是唯一的「在哪」 |
| `text` | 解出来的原文 |
| `confidence` | 事后判断这次是不是蒙的 |
| engine + 模型摘要 | 换模型会改变结果,不记就无法复现旧 run |
| 耗时 | OCR 是唯一量级在毫秒的操作,预算与超时都靠它 |
| 周期序号 | 与 `hit` 一样,证明它属于这一帧 |

点击一侧要能连起来:`page → reading(选中的那个) → click`。今天 `Action` 事件带
`outcome / sadScore / maximumSad / matchedRect`,读取路径没有分数,所以 text +
confidence + rect 是它的替代证据。

**OCR 需要独立的预算维度,不要并进像素池。** 判状态会出现在 **wait 循环**里——
「等卡片展开」意味着每个周期读一次。模板匹配每周期跑很正常,OCR 每周期跑是另一个
量级(实测 2–13ms/行)。而今天的像素预算是**一个共享池**,按锚点顺序扣减,谁耗光就
报错点名**当时正在搜的那个锚点**(哪怕元凶在前面)。把 OCR 折算成像素比较塞进同一个
池子,会让一个 OCR 重的页面**悄悄饿死**锚点匹配,并且报错点名一个无辜的锚点。单位
不可比,两种工作合用一个数,那个数就同时说不清任何一种。最简形式:每周期最多读 N 个
区域,或一个毫秒截止。

## 五、开放问题

1. ~~**appearance 与能力的关系。**~~ —— **已关闭(2026-07-31),见 §四之二.4。**
   原文担心「暗色形态可点、亮色形态是禁用态不可点」会逼 appearance 携带能力差异。
   答案是那不是 appearance:**它是状态**,按 §四之二.4 (a) 处理——有名字的 appearance,
   脚本读命中身份自己决定点不点。appearance 的定义边界(换外观,不换位置、不换意义)
   把这个情况排除在列表之外,所以它永远不会逼出能力差异。
2. **per-reference 的 searchRoi 细化。** 今天没有一个页面需要它。同上,有实例再做。

   > **已裁决(2026-07-31):`searchRoi?` 是可选细化,缺省继承元素默认值**——即 §2.2
   > 结构块写的那个形状,不是必填。下面这段保留为实现现状的说明。
   >
   > 补充(2026-07-31):
   > `AuthoringPlacement.searchRoi` 今天存在、已经通到运行时(编译器按放置展开,
   > 每个 element 带那一页自己的 ROI),而且 workbench 已经能按页改它——
   > `InteractiveRegion::setSearchRoi`(`edit-page.cpp:773-791`)写的就是本页
   > placement,注释还点名了它取代的那个「改一页动全部」的缺陷。只有 CLI 没有。
   >
   > 所以真正开放的不是「要不要有」,而是:**`PageReference` 里这个字段该是必填还是
   > 可选细化?** 今天它是必填的 `PixelRect`,新建时从元素默认值播种;而 §2.2 的
   > 结构块写的是 `searchRoi?`,含义是「缺省即继承元素默认值」。两者是不同的设计,
   > 而且加进 `exercised` 之后还多一层:行使 `identify` 的引用能不能细化 ROI?
   > (页面签名今天走的是元素级 ROI。)这一条不要当成已完成。
   >
   > 另注:`authoring-document.hpp:207-209` 还写着「each placement may refine it per
   > page in a later phase」,这句话已经过时了——那正是原文误以为「只是留了位置」的
   > 来源。
3. ~~**`Read` 的 OCR 参数面。**~~ —— **已关闭(2026-07-31):定在标注期。**

   > 单行/整块和字符集限制都是**标注期**参数,不由脚本调用时传。
   >
   > 决定性的理由是确定性,不是「哪个方便」:否决条件 4 要求同一份观察序列加同一个
   > seed 必须产出同一份动作序列。字符集若由调用方传,两个脚本就能从**同一帧像素**
   > 解出不同的值——那是**策略在改证据**,站在「C++ 拥有所有保证」的反面。阈值和
   > 颜色键早就因为同样的理由是标注期事实,字符集是同一类东西。
   >
   > 落地形状:`ReadLayout { SingleLine, Block }`,以及
   > `optional<CharsetRestriction>`,缺省表示不限制。`CharsetRestriction` 目前只有
   > `Digits` 一个枚举值——那是唯一实测到的需求(`sortie_level` 读的是 `Lv.65`)。
   > 单值枚举是故意的:它可扩展,而 `bool digitsOnly` 不可。
4. ~~**`Owned` 的两种读法。**~~ —— **已关闭(2026-07-31):`Owned` 读作「家在这一页」。**
   另一种读法——「作者声明这个元素只属于这一页,工具据此在别处引用时拒绝」——不可能
   成立:`runAddElement` 把每个画出来的元素都标成它那一页的 `Owned`,照字面执行等于
   「`page reference` 永远失败」,§2.4 那个 `back` 被两页引用的例子首当其冲。真正守住的是
   **唯一所有者**:同一个元素最多一行 `Owned`,其余用它的页面一律 `Referenced`。
   **持有不是独占**;真要声明独占,那是元素上的另一句话,不是给这个枚举加第二种含义。
5. **拥有元素的那一页钉不了 appearance。** `page add` 在画元素的同一次编辑里就写了拥有页
   的引用,而 `page reference` 拒绝已有引用的页面,所以第二个 appearance 出现之后,拥有页
   只能跨 appearance 折叠(结果正确,但每周期多搜一次)。补法二选一:一个
   `page pin ROOT PAGE ELEMENT NAME` 动词,或让 `page reference --appearance` 更新已有的
   那一行。现状由测试 `the page that owns the element still folds across both` 钉住。
   这一条会随模型上移第二层重新提问,但今天没有答案。
