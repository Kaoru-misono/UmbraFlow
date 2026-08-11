# 目标形态 — 三层系统与 Agent 操作者

> 状态:**已定稿,2026-08-01 开发者审定。**
>
> 本文是顶层形态文档。骨架沿用[三层 Task System](2026-07-29-three-layer-task-system.md)
> ——修订,不推翻;[标注模型重构](2026-07-31-annotation-model-capabilities.md)在本文
> 中的位置是层间合同。「C++ 拥有所有保证,Luau 拥有所有策略」在每一处裁决中仍然
> 有效。
>
> 本文刻意不引用现有代码。先钉目标形态,审定后再对照代码盘点复用(见 §十)。
>
> **2026-08-01 晚**:与[页面模型上移到脚本层](2026-07-31-script-owned-page-model.md)
> 融合修订过一轮,修订已折进正文,不另立注记。
>
> **2026-08-12**:那份文档已作废。正文 §一、§五、§七、§十 里「归属以那份为准」几处
> 记的是当时的裁决,按当时读;运行时模型现在由
> [`schema/umbraflow-runtime-v2.schema.json`](../../schema/umbraflow-runtime-v2.schema.json)
> 与[运行时模型合同](2026-08-09-runtime-model-contract.md)共同定义,§九 的退役清单
> 已执行完毕。

## 一、系统是什么

一个游戏自动化脚本处理系统:开发者用自然语言教,Agent 标注并学会玩,业务脚本在
运行时无人值守地执行。

### 1.1 三层

- **C++ 底层**:提供事实与动作,守住保证。事实:这一帧是什么页面、某元素在哪、
  区域里的文字是什么、某坐标命中了什么。动作:点、按、滚、拖(按模式装载,
  见 §七)。保证:动作授权、观察新鲜度、证据留痕。这一层不懂业务。它的**原语面**
  是 `cycle_match` / `cycle_read` / `cycle_click`,加输入原语与项目文件读写;认页、
  找元素、同类枚举、语义等待都在框架层,踩着 `cycle_match` / `cycle_read` 实现
  (归属以[页面模型上移到脚本层](2026-07-31-script-owned-page-model.md)为准)。
  所以本文各节写的「底层事实」按**能力**归属读:那些事实确实存在、确实由 C++ 的原语
  支撑,但它们住在哪一层看那份文档。
- **框架层(Luau)**:两个面孔。对业务脚本:加载与调度、动词供给、等待语义——
  约束第三层,同时供给第三层。对 Agent:操作台,允许 agent 进行更高权限的操作。
- **业务层(Luau)**:流程编排。懂每个页面是干什么的、分支怎么走、点哪里进哪个
  页面。

### 1.2 Agent 是第三种操作者,不是第四层

系统原有两种操作者:任务脚本、人。Agent 是第三种,与前两者并列:独立的前端,
自己的动词表与事件词汇。自由点击这类特权不往另外两个前端上挂。

Agent 做两件事:替开发者标注;被开发者教会玩游戏。它的产物只有两种文件——标注
工程与业务脚本。

## 二、两种信任模式

鸡生蛋问题:全新的游戏没有任何标注,Agent 要标注就得先点进各个页面看,而运行
规则要求只能点标注过的东西。解法是按操作者身份分成两种模式:

|          | 运行模式(业务脚本)      | 探索模式(Agent) |
| -------- | ----------------------- | ---------------- |
| 点击     | 仅标注元素(可信第二层执法) | 任意坐标      |
| 像素     | 永远拿不到              | 区域像素随便取   |
| 帧差异   | 无                      | 可查             |
| 留痕     | 全部进 trace            | 全部进 trace     |

两条边界规则:

1. **业务脚本只拿语义结果**——页面名、文字、命中与否。像素判断是底层的保证,放进
   脚本就成了无法证伪的策略。
2. **特权动词只存在于探索环境。**业务环境根本不装载它们,而不是装载后逐次拒绝。

**执法者是谁。** element 与 page 住在框架层,所以 C++ 的硬保证收敛成四条——hit 来自
**本票据**的 `cycle_match`(同帧)、观察租约新鲜度、指纹兼容、票据单次消费。「业务
脚本只点标注元素」不消失,只是执法者不是 C++ 而是**可信框架层(第二层)**:裸点击
原语以闭包 upvalue 只交给框架层,业务环境暴露的是「点元素 / 点容器内选中项」这类
包装,裸点击**根本不在那张表上**;探索环境(Agent 前端)才装载裸点击。这与上面
第 2 条是同一条纪律,只是它现在也管住了坐标点击本身。

## 三、Agent 的通道:Luau,单通道

Agent 探索与业务脚本用同一套 Luau 动词,探索环境多几个特权动词。Agent 的操作以
Luau 片段的形式加载执行,逐条调动词(截一张 → 看 → 点一下 → 再截)是主工作流,
已在此前的标注会话中验证过。

这样做的收益:Agent 玩游戏的过程就是对脚本层 API 的持续测试。Agent 玩不动的
地方,业务脚本将来也一定写不动,问题暴露在标注期而不是任务上线之后。

运行中把探索过程固化成业务脚本:缓,等主流程稳定后再商榷。

## 四、教学与沉淀

本文建议:教学文档保持粗糙散文(样例:仓库根目录《标注过程.md》),消化歧义是
Agent 的职责;要规范的是 Agent 一侧。教学格式本身要不要再收紧,列入 §九。

**消费协议**:拿到教学后逐步跟着玩;截图验证教学里的每一句断言;标注;证伪;
验证不了或拿不准的,列成问题问回开发者。教学里「白色感叹号可能也可以作为识别项」
这类句子,正确流向是 Agent 验证后回答,不是开发者替它想清楚。

**产物清单**:一份标注工程(页面、元素、能力)+ 一份业务脚本骨架 + 一张回给
开发者的问题清单。

**沉淀原则**:对话可丢,产物必须齐。任何时刻丢掉对话历史,凭教学文档 + 标注
工程 + 业务脚本即可重建系统行为。标注工程本身也要以可重放的形式产出——此前的
author-*.ps1 重建脚本是这条原则的既有实践。

标注手艺——模板避开会变的像素(裁掉汉堡上的红点)、阈值怎么定、颜色键怎么选、
怎么证伪——写进 Agent 的操作手册,不进开发者的教学。

## 五、标注是层间合同

标注模型(能力集合、页面引用、appearance)是三层与 Agent 之间的合同:Agent 生产
它,系统拿它执行与把关,业务脚本引用它的名字。模型的**实现位置**按[页面模型上移到
脚本层](2026-07-31-script-owned-page-model.md)住在框架层,模型的**设计结论**一条
不改。能力模型与底层动词一一对应,这个对应从纯需求推出,不依赖任何现有代码:

- identify ↔ 认页
- interact ↔ 点击
- read ↔ 区域信息
- 命中判断 ↔ 拿标注当查询目标的反向查询(场景待定,见 §九)

本文对[标注模型文档](2026-07-31-annotation-model-capabilities.md)追加四条裁决,它们在
归属上移之后原样成立:

1. **极性在引用侧(已确认)。**required / forbidden 属于页面引用,不属于元素。
   随之 Identify 成为空能力——开发者曾质疑「里面什么都没有,有点奇怪」;本文的
   回应是序列化为名字列表(`capabilities = ["identify", "interact"]`),文件里
   不出现空表。空能力这半条待审定。
2. **appearance 永远不携带能力差异。**能力差异按 §六的三分法建模。该文档开放问题 1
   就此关闭。
3. **脚本可以 find 当前页面引用的任何元素;click 才要求 interact。**状态元素
   (identify-only)必须能被脚本查询;find 是只读的,放开无害。
4. **元素声明自己的验证来源:模板 appearance,或期待文字。**有字的可点元素用文字
   验证——矩形 + 期待文字「出击」+ 能力,运行时 OCR 该矩形、核对通过才点,不再
   需要像素模板、颜色键与阈值;没字的(汉堡、小房子、加速图标、旗帜)仍用模板。
   验证来源对脚本层无感,脚本照旧 `click(出击)`。期待文字写在标注里——「这一格
   是出击」是关于 UI 的事实,不是调用策略——印证该文档开放问题 3 的倾向。明确
   不做的一半:运行时不创建可点对象,标注期声明、运行期核对;动态内容见 §七的
   容器点击。

   期待文字是**数据**,核对是**策略**:文字写进项目文件所以可证伪、可重放,而
   「怎么算匹配」(全角半角、繁简、包含还是相等)的核对逻辑归框架层 Luau,C++ 的
   `cycle_read` **不收** `expected` 参数。这与
   [标注模型文档](2026-07-31-annotation-model-capabilities.md) §四之二.3 并立成立,
   不冲突:一个说事实住在哪,一个说规则归谁。

   > **更正(2026-08-01,落地时)。** 期待文字最终**也可以写在引用侧并优先于元素侧**,
   > 所以「元素声明自己的验证来源」这半句要说准:**元素**决定用哪一类证据——有
   > appearance 就按像素,没有就按文字;**页面的那一行**决定这一页该读出什么。分法不是
   > 实现方便,是因为共享的标题栏每页读出的字不一样,只有页面知道。见 §九-8。

## 六、建模三分法:页面 / 有名字的 appearance / 无名 appearance

判据不是视觉差异,是谁需要知道:

- **页面 = 流程状态。**整个「接下来能做什么」都换了才拆页面。例:战斗员配置的
  未完成/就绪两态,脚本在此整体分叉——去选人,或点进入。
- **有名字的 appearance = 局部事实。**同一矩形上互斥的状态是**一个**元素带一张命名
  appearance 列表,不是 N 个元素——把 `speed_1x` / `speed_2x` / `speed_3x` 拆成各自
  独立的元素这个替代方案**已否决**;机制沿用[标注模型文档](2026-07-31-annotation-model-capabilities.md)
  §四之二.4(a)。例:`speed_button` 的 `speed_1x / speed_2x / speed_3x`,`auto_button`
  的 `auto_on / auto_off / auto_gray`。命中身份上到脚本面:
  `hit.appearance == uf.appearances.speed_3x`。战斗页始终是一个页面,不随速度与
  自动战斗的状态组合爆炸。
- **无名 appearance(曾称 variant)= 脚本永远不需要区分的外观差异。**例:back 按钮
  暗底白字与亮底黑字。局部小变化(汉堡上时有时无的红点)不用 appearance,用裁剪:
  模板避开那块像素,一次搜索;appearance 留给整体翻转、裁无可裁的情况,代价是每多
  一个 appearance 最坏多一次搜索。这个代价只落在**状态读出**那一类(上一条);
  「哪套形态适用」由页面决定的那一类在引用侧钉死 appearance,那一页每周期只搜 1 次。

一句话测试:脚本会不会问「现在是哪个」?会——有名字的 appearance;从不问——无名的
appearance,或干脆在引用侧由页面钉死;连下一步动作全集都变了——页面。

推论:「点了没用」是策略,不是保证。自动战斗按钮变灰时点击无效,但保证只有「点的是
标注过的东西」(执法者见 §二);要不要点,由脚本先读 `hit.appearance` 是不是
`auto_gray` 再决定。

典型脚本形状:

```lua
local hit = find(speed_button)
while hit.appearance ~= uf.appearances.speed_3x do
    click(hit)
    hit = find(speed_button)
end
```

## 七、动词清单

本节是**能力面**——脚本能用的动词全集。物理归属(哪几个是 C++ 原语、哪几个在框架层
用 Luau 踩着原语实现)见[页面模型上移到脚本层](2026-07-31-script-owned-page-model.md)。

底层事实(运行模式可用):

- 认页:当前帧解析为哪个页面
- 找元素:本页引用的元素,命中与位置;验证方式按元素声明执行(模板匹配,或
  OCR 读出该矩形——「读出来的算不算匹配」的核对逻辑在框架层,见 §五 裁决 4),
  按需进行,不预扫全部矩形
- 同类枚举:区域内某类元素的全部命中,返回 (类型, 位置) 清单。这是底层唯一
  全新的匹配机制——从找一个到找全部（都需要给定查询范围）——近期最大的一块新增工作量,小地图近期方案
  整个压在它上面
  > **更正(2026-08-05)。**「小地图近期方案整个压在它上面」被实测推翻:同类枚举一行
  > 没写,小地图近期方案已经在 `uf-chaos/tasks/daily.luau` 里跑起来了。因为那一片的
  > 几何是已知的——锚点定位、加一个列距 95、五行固定 y——于是五行四类打 20 次单点
  > `cycle_match` 就够,不需要「找全部」。
  >
  > 它现在是一个**有条件的**工作项:只有真机量出展开地图页的节点**不**落在规则格点上
  > 才做。真做的话要连带解决三件——非极大值抑制、自己的预算、超限响亮报错(照
  > `read_lines` 那条,绝不返回截断的前几个)。见
  > [全图规划要的框架能力](2026-08-05-map-verbs-and-connectivity.md)第四节。
- 读区域:文字与结构化信息。**已落地(2026-08-01)**:两种布局,由调用方选。
  单行是「我画了这个矩形,读它」(`cycle_read` / `observe.read_element`);整块是
  「我不知道里面有什么,把行找出来」(`cycle_read_lines` / `observe.read_lines`),
  跑检测模型,每行带自己的矩形(目标像素)。整块是为「内容会动的区域」存在的——
  连续滚动的角色网格里,一个名字的位置是**帧的属性**而不是模型的属性,所以标注区域、
  读出内容。预算按「定位算一次、每找到一行再算一次」从同一个读预算扣;超限是响亮的
  `RecognitionIncomplete`,绝不返回截断的前几行。
  **(2026-08-03 补)** 空读的含义由调用方给,动词不替它定:区域本来就空,和内容还没画
  出来,读出来是同一片空白,一帧分不开。两个读动词因此各多收一个无默认值的参数。传
  `observe.empty_is_absence` 时空区域就是答案,`forbidden` 子句要的正是它;传
  `observe.empty_is_unknown` 时抛 `RecognitionIncomplete`,调用方再观测一次。这条是
  「停下来的搜索不算没找到」在读这一层的同一条规则
- 命中判断:坐标 × 标注 → 命中与否(场景待定)

底层动作(运行模式可用):

- 点击:锚定标注元素,区域内偏移。动态内容(命运选择的三张卡片)锚定标注的
  容器区域,偏移由运行时在容器内读出的位置计算——动态的是偏移,不是标注;授权
  仍锚在静态的容器上。**已落地(2026-08-01)**:`observe.read_lines` 返回的是
  `Hit`(`positioned_by = "text"`),走原来那扇 `observe.click`,收据、页面、
  `interact` 三道检查一条不减;新加的第六道是「这个 hit 是哪一帧上定位的」——
  行的位置只对读它的那一帧成立,而它到宿主时是一对裸坐标,C++ 没有句柄可查。
  偏移是**任务的数字**,机制是框架的:`hits.offset(hit, dx, dy)` 只从框架自己
  铸过的 hit 派生,并继承它的周期
- 按键
- 滚轮:倾向锚定标注区域,待实例检验(见 §九)
- 拖拽:~~运行模式暂不提供,随远期小地图立项再定~~
  > **已立项(2026-08-05)。** 远期小地图就是那个立项,见
  > [全图规划要的框架能力](2026-08-05-map-verbs-and-connectivity.md)第二节。形态定为
  > `drag(start, offset)`,**一次调用完成整个手势**——因为每个动作动词都消费周期,拆成
  > 按下/移动/抬起就是三个周期三帧租约,中途必然撞 `StaleObservation`,失败时还会留下
  > 一个按住不放的鼠标键。端口沿用 `cycle_long_press` 那条「每条退出路径都释放」的保证。
  >
  > 起点锚在哪还要裁决,倾向锚在一个标注的画布区域上:那和上面点击那条「命运选择三张
  > 卡:锚定容器区域、偏移由运行时算」同构,现有授权规则一行不改,而且画布元素有现成
  > 先例(`menu_burger` 就是只有矩形、没有模板、页面上 exercise `interact`)。
  >
  > 顺带记一笔实现现状:投递层的 `controller::movePointer` 本来就会读按住的键来决定发
  > 普通移动还是拖拽,今天发不出去只是因为端口层不让任何键跨调用保持按下。这是补一个
  > 动词,不是补一套机制。

探索特权(仅 Agent):

- 自由坐标点击、自由拖拽
- 区域像素读取
- 帧差异查询

框架层:

- 等到(页面 == X)/ 等到(出现 元素):反复观察,连续 K 次成立才算到,超时报错
- 点到状态为止:点击加状态元素查询的循环封装
- 任务脚本加载与调度

明确不做:

- **全帧稳定等待。**常驻动画(休息点的炫彩边框星星)使全帧差异永不归零,该动词
  必然死锁或误判。教学中四处「等待帧稳定」全部改写为语义等待。
- **C++ 内路线规划。**规划是策略,归脚本与 Agent。
- **运行模式的原始像素。**

## 八、小地图:两级,先近后远

近期(够玩通):不放大、不拖动。在分支选择区做同类枚举——红双剑、黄篝火、灰问号
各在哪——脚本按类型优先级挑一个点。教学第 12-13 步的实际玩法只需要这些。

远期(全图规划):放大、左右拖动、跨屏拼接、节点拓扑。单独立项,等近期跑通再
评估。届时边界不变:C++ 只提供「这一屏看见哪些节点」与拖拽动词,路线选择仍在
策略层。

> **已立项(2026-08-05),边界原样成立。** 框架侧见
> [全图规划要的框架能力](2026-08-05-map-verbs-and-connectivity.md);这个游戏要量什么、
> 标什么、脚本怎么写在工程目录 `E:\umbraflow-projects\uf-chaos\MAP.md`。
>
> 「C++ 只提供这一屏看见哪些节点、路线选择在策略层」这条边界不但成立,还比预想的更值钱:
> 因为规划的输出是**语义**(「第 3 层走第 2 个节点,那是篝火」)而不是坐标,拼接完全不
> 需要跨周期的坐标身份,§七点击那条新加的第六道检查一行不用动。选路那一帧重新观察、
> 重新找、点一个新铸的 hit 就是了。
>
> 上面「近期」那段里「在分支选择区做同类枚举」也已被实际实现改写:实现走的是「按锚点
> 算出格子 + 逐格单点匹配」,一次都没用到枚举。见 §七 同一日期的更正。

## 九、开放问题

1. **命中判断的场景。**Agent 自检刚标的元素,还是重放分析?接口不同,待定。
2. **运行中固化脚本。**Agent 边玩边把探索写成业务脚本,缓。
3. **同类枚举的证伪。**单元素的证伪是该命中的命中、该落空的落空;枚举的证伪矩阵
   长什么样(数量、位置、误检),没有先例,做时再定。
4. ~~**探索模式的 trace 词汇。**~~ —— **已定(2026-08-01,随工单 4b 落地)。** 两个
   `annotation.*` 事件:`annotation.click_delivered` 与 `annotation.region_saved`,
   各带被碰到的点或矩形,裁剪那条另带 PNG 的 sha256 与字节数——像素本身流向 Agent
   而不是流进证据流,哈希是把 Agent 后来写出的文件系回那一帧的唯一凭据。刻意不折进
   `engine.*`:裸坐标既没有元素也没有页面,写成 `engine.action_delivered` 等于在记录
   里放进一次从未发生的识别。
5. **滚轮的授权语义。**倾向锚定标注区域,与点击同构,待实例检验。
6. ~~**远期小地图立项。**~~ —— **已立项(2026-08-05)。** 见
   [全图规划要的框架能力](2026-08-05-map-verbs-and-connectivity.md)与工程目录的
   `uf-chaos/MAP.md`。要建的是两个动词(拖拽、读连通)加一套拼接图评估;同时定下三条
   **不做**:帧差异原语(改真机标定一次拖拽走多少像素)、通用线段检测(收窄成「给定
   两点问有没有连线」,因为列表型结果的证伪矩阵仍是本节问题 3)、跨周期的坐标身份
   (语义输出让它不必要)。
7. **教学文档要不要收紧格式。**本文建议保持粗糙散文、只规范 Agent 侧;开发者
   曾表示后面可以再规范,待定。
8. ~~**文字当页面签名证据。**~~ —— **已裁定(2026-08-01,开发者):门开。** 出击页的
   识别项本来就是左上角那两个字,现在「这一格读出出击」就是 identify 证据。四条规则:

   - 没有 appearance 的元素可以行使 `identify`,条件是它声明 `read`,而且**有期待文字
     可核**;两样都没有仍然拒绝,措辞不变。
   - 期待文字可以写在**引用侧**(`[[reference]] expected_text`),优先于元素侧。动机是
     本作每一页都把页名印在同一个左上角矩形里:一个共享元素 + 每页一行「这里读什么」,
     不必为每页各裁一张模板、各定一个阈值、各证伪一次。
   - 核对是**相等**不是包含(先去掉首尾空白),否则「出击」会认成「出击纪录」——同一个
     矩形上一个页名是另一个的前缀,包含式判定会把后一屏认成前一页并照着点。
   - 置信度低于 `read_floor`(基点,元素级,缺省 8000)的读出**不算证据**,记为「没读到」,
     绝不记为命中。OCR 对任何矩形都会返回像样的文字,置信度是唯一的守门。

   有 appearance 的元素一个字节都没变:它照旧按像素认页,引用侧的期待文字对它只是一条
   关于「读出什么」的事实。

   **证伪跟着长出了文字那一半。** 矩阵的一格可以声明这块区域**读出什么**,判分时把
   置信度与判决并排报出;而**同一个元素被声明在两块屏上读出同一段文字**是一条
   confusion——那正是「这段文字区分不出这两屏」。唯一的例外是两块屏都**声明自己是
   同一页**:一页被拍了两次不是混淆(连续滚动的网格拍两个滚动位置,标题当然一样)。
   屏因此多了一个可选字段「我是哪一页」,它不是豁免而是**换来一次度量**——声明了哪
   一页,那一页就必须在这块屏上解析成功。可选是因为探索时**先有像素后有页面**,必填
   只会逼作者现编一个页面来满足它。

   落地在 `model.luau` / `observe.luau` / `project.luau` / `scribe.luau`(模型、读取与
   写入)和 `oracle.luau` / `regress.luau` / `recognition.luau` / `reading.luau`(屏、
   期望与判分);[标注模型文档](2026-07-31-annotation-model-capabilities.md) §四之二.2
   的「构造时应拒绝 `identify`」随之更正。

## 十、下一步

1. 复用/退役清单由[页面模型上移到脚本层](2026-07-31-script-owned-page-model.md) §九
   承担。本条原本要做的那次全量盘点——对照现有代码分出哪些直接是零件、哪些要改、
   哪些放弃,并重启此前挂起的八个代码级问题(评估缓存、派生 recognizer 去留、双
   schema 升版等)——退居其后,作为退役时的爆炸半径参考。

2. 文档修订只考虑 plan 和必要文档:三层文档受影响的小节(操作者、动词表)、
   标注模型文档(§五的四条追加裁决)。知识类文档已于 2026-08-01 **全部删除
   (已执行)**——在代码框架未稳定时写知识文档只是徒增维护负担;可复用的失败知识
   留在 `docs/pitfalls/`。

3. 迁移按[页面模型上移到脚本层](2026-07-31-script-owned-page-model.md)执行:模型上移
   框架层,项目文件格式由框架层定,标注模型文档里那份「两个 schema 一起升版」的迁移
   计划**作废**。动词清单以本文 §七 为准。

## 2026-08-03 — Recognition asks one direction only

Moved out of the `recognition.luau` header, which now cites this section from
`recognition.verify`. It records a ruling and the alternative it rejects, not a
constraint the code has to be reminded of.

`recognition.verify` resolves the page a screen DECLARES, on that screen, and
asks nothing in the other direction. "A screen declaring page P must resolve NO
OTHER declared page" is false on this model: a page signature names the marks
that IDENTIFY a page and is never an inventory of its screen (`oracle`, "three
states and not two"), so a card-detail overlay screen resolves the battle page
underneath it, correctly. Nothing in the model says which page an overlay sits
on — there are `push` and `pop` edges and no base relation — so the exception
could not be written down.

The cost is also real: resolving every declared page on every screen multiplies
a check's reads by the page count, and that budget is fixed from the file before
the VM exists (`entry/cli/check.cpp`).

What that direction reached for is already measured per element: an appearance
hitting a screen it does not own is a misfire cell, two appearances hitting one
screen is `ambiguous_appearances`, and one region reading one text on two pages
is the repeated-text confusion the `recognition` header states first.

## 2026-08-04 — The other direction is measured, and still not judged

The ruling above stands unchanged: "a screen declaring page P must resolve NO
OTHER declared page" is false, and nothing in the model can yet write down the
exception. `regress` now sweeps every declared page against every screen anyway
and reports what resolved (`recognition.sweep`, and the `resolution` rows of the
verdict), because the ruling rejected the *finding* and the cost, not the
question. Neither objection survives as stated:

- **Not a finding.** The rows sit beside `separations` -- measured, reported,
  and never counted into `accepted`. The exit code is findings alone.
- **The cost is now a number.** It is measured rather than budgeted for; see
  below.

What the sweep buys is the thing "already measured per element" could not
supply. On the reference project it reported one page resolving on 59 of 85
screens, and no per-element rule can say that. Its anchor is a required
`seasonevent_crest` and a forbidden `battle_draw`: the first matches on 60
screens, the second on the one screen that subtracts, and the page resolves on
the 59 that remain. Every one of those cells is UNCLAIMED -- the file declares
nothing about either element on any screen -- so there is no per-element rule
there to fire or to stay silent. Only the conjunction, asked of a whole frame,
has the number.

**What the number turned out to be, and what it bought.** Measured on the
reference project (85 screens, 331 elements, 87 pages), release build: the sweep
costs 6903 of the check's 7082 text reads and about 40 s of its wall clock,
nearly all of it OCR. Two things followed.

`task::CycleAnswers` answers one rectangle once per frame, because pages share
elements -- five of this project's pages identify by one `page_title`. It removes
1060 of those 6903 reads and 1200 of 3495 template searches, 125.6 s to 108 s,
and it is transparent: the whole 29,400-row report is byte for byte what it was.

The read budget the bullet above described is GONE from `check`. It was sized
from the file (`readBudgetForCheck`, `recognition.reads_per_sweep`), and sizing
it at all was the mistake: a check's cycle is the whole of one screen's work, the
frames arrive one file per capture so the walk cannot re-open one, and the sweep
resolves pages in model order -- so a budget exhausted part-way through raises
out of `observe.resolve_page` and turns an ordinary `unresolved_page` finding
into a failed run. The run is bounded by wall clock (`maxScriptRuntime`) and the
per-cycle ceiling is unreachable by construction.

And the sweep is now OPT-IN at the CLI (`umbra-flow check --sweep-pages`),
because 40 s of a 110 s check is a measurement that moves no exit code. Off, the
walk still resolves the page each screen DECLARES through `recognition.verify`,
so every finding is unchanged; what is missing is the `resolution` and
`page_coverage` rows, and the summary OMITS `resolutions` and `pages_unresolved`
rather than reporting zero -- "not measured" and "measured, none" are different
facts. `recognition.needs_engine` takes the switch for the same reason: without
the sweep, a page no screen names is never resolved and cannot want an engine.

The exception the ruling could not write down is the `over = [...]` field of
[状态层与策略插槽](2026-08-04-state-layer-and-policy-slots.md) §3.4. When a page
can declare what it covers, a co-resolution that `over` does not explain becomes
a finding. Until then it is a fact about the corpus.

## 2026-08-03 — `cycle_read_lines` is a verb, not a flag on `cycle_read`

Moved out of the `cycleReadLinesFn` header in
`modules/task/source/task/ffi/uf-tables.cpp`, which now cites this document. It
records a rejected alternative, not a constraint on the primitive.

A flag on `cycle_read` was rejected because what comes back differs in kind, and
the two callers assert opposite things: `cycle_read` says the rectangle holds a
single line, `cycle_read_lines` says it does not know what the region holds.
