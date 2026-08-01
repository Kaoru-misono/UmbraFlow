# 页面模型上移到脚本层 — 目标架构

状态:开发者裁决已定,代码未动。本文是落纸,不是提案。2026-08-01 融合修订见 §十二。

本文修改 [`2026-07-29-three-layer-task-system.md`](2026-07-29-three-layer-task-system.md)
的第一层边界,并作废
[`2026-07-31-annotation-model-capabilities.md`](2026-07-31-annotation-model-capabilities.md)
中关于模型归属的部分。能力模型本身的设计结论仍然有效,只是它的实现位置从 C++ 变成
第二层 Luau。

## 一、为什么改

2026-07-31 用能力模型完整标了一轮 卡厄思梦境 的战斗页和卡牌详情页。三个问题在同一天
撞到,形状是一样的。

**页面之间不是互斥的画面。** 卡牌详情就是战斗画面加了一层浮层。实测:弃牌堆、汉堡、
结束回合按钮全都还在,只是变暗,`battle_discard_pile` 在那一帧上 SAD 25120,天花板
142290,命中。同一个元素在没有浮层的战斗帧上得 0 分。矩阵为此学会了签名是合取式
(见 `d489979`),但那只解决了判分。脚本侧看到的仍然是三个互不相干的页面。

**识别把授权卡死了,而这不是任何人决定的。** 站在卡牌详情画面上,结束回合按钮就在那儿,
点了会生效,但 `battle` 这一页解析不出来(它的 forbidden 子句触发了),于是授权消失。
这个行为可能是对的,但它是模型耦合出来的副产品。

**元素的粒度对不上。** 战斗员选择页是鼠标点选加可滚动网格,同一个元素在屏幕上有 N 个
实例,位置随滚动变,数量不定。今天的 element 就是一个矩形。

三个问题的共同点:C++ 对页面是什么有固定意见,而游戏不按那个意见长。每撞一次就要改一次
C++,而下一个游戏还会再撞一次。

还有一条更基础的:**系统唯一知道自己在哪的办法,是把每一页的每一个识别标记都搜一遍。**

```cpp
// modules/annotation/source/annotation/recognition-runtime.cpp:435
auto const anchorOrder = catalog.pageAnchorOrder();
for (auto const id : anchorOrder) { ... }
```

自动化不是这么工作的。你从一个已知状态出发,点了一个已知的按钮,你**知道**自己应该到
哪一页,只需要验证一下。全量识别是出错之后的回退路径,不该是常态入口。

## 二、裁决

**裁决 1:element 和 page 上移到第二层。** C++ 只提供更底层的能力:给一个矩形返回像素,
给一个矩形和坐标交付点击,给一个矩形返回 OCR 结果。标注侧和运行时识别侧都在脚本层。

**裁决 2:模型归第二层,不是第三层。** 第二层是 `modules/task/runtime/*.luau`,随二进制
发布,是可信的。它提供 element 和 page 的基础模板,第三层继承和扩展。这样 trace 的身份
仍然稳定,判断工具仍然是我们提供的,而第三层拿到的是自由度而不是义务。

**裁决 3:继承按 Luau 业界做法。** metatable 加 `__index`。第三层把自己的表改坏是脚本
作者的事。第二层负责检查关键字段。

**裁决 4:项目文件格式由第二层定,支持扩展。** 项目自己加的字段项目自己用。

**裁决 5:第一层提供项目文件读写能力。** 没有它,脚本产出的模型落不了盘。

## 三、三层的新边界

`2026-07-29` 写的是这样:

```text
C++ 保证层
    modules/engine   原子能力:observe / resolvePage / findAction / act,以及授权
依赖方向: engine -> annotation
```

`resolvePage` 和 `findAction` 是模型层在 C++ 的证据,`engine -> annotation` 这条依赖同理。
改成:

```text
C++ 保证层
    modules/script   Luau 底座:VM、沙箱、配额、指令与时间预算、中断取消
    modules/vision   模板匹配:给模板和搜索矩形,返回位置和分数
    modules/ocr      文字识别:给矩形,返回文字
    modules/controller  捕获、输入交付、目标解析
    modules/engine   原子能力:observe / match / read / act,以及票据与授权
    modules/task     TaskHost、票据账本、trace sink、generation、项目文件读写

Trusted Luau Framework            modules/task/runtime/*.luau,随二进制发布
    task 生命周期、step、观察周期编排、wait、retry、interrupt、错误翻译
    element 与 page 模型、页面图、确认与识别、证伪判分、项目文件读写

Project Task                      <project>/tasks/*.luau
    一个游戏的页面定义、页面流转、条件分支、操作顺序、弹窗处理
```

依赖方向去掉 `engine -> annotation`。

## 四、第一层能力面

票据协议不变,`2026-07-29` 第四节的观察周期与账本硬规则原样保留。变的是周期内能做什么。

```text
cycle_open(deadline)              -> ticket        一次 capture
cycle_close(ticket)               -> ()            确定性释放,幂等
cycle_match(ticket, 模板, 矩形)   -> hit | nil     纯模板匹配,给位置和分数
cycle_read(ticket, 矩形)          -> text          OCR
cycle_click(ticket, hit | 点)     -> receipt       消费周期
```

`cycle_page` 和 `cycle_find` 取消。它们在第二层用 Luau 重新实现,踩在 `cycle_match` 上。

项目文件读写是周期之外的能力:

```text
project_read(名字)   -> blob
project_write(名字, blob)
```

必须做路径 confinement,实现可以复用 `entry/input-agent/path-validation.*`。

## 五、保证不骑在 Luau 表上

裁决 3 说第三层改坏自己的表是它自己的事。这句话之所以安全,有一个结构上的原因,必须写
下来,因为它不是自动成立的。

票据账本在 C++。fingerprint 和观察新鲜度是在 `cycle_click` 的时候对着票查的,不是从
脚本传进来的表里读的。所以第三层把 page 表改烂,最坏结果是它自己的脚本跑错,不可能变成
一次落在错位置的点击。

**任何把 fingerprint、新鲜度、票据身份缓存进 Luau 表的优化都越过了这条线。** 不要做。

## 六、检查

第三层的错误分三档拦,从早到晚。

**Luau `--!strict` 类型检查。** 第二层发布 `Page` 和 `Element` 的类型定义,第三层在
strict 模式下分析,漏字段或类型写错在分析期就红。继承用 `__index`,约束用类型,两者配套。

**pre-VM AST pass。** 已经存在:`modules/task/source/task/script-validator.hpp`,在任何
VM 被创建之前解析脚本,枚举每一个 `uf.elements.<name>` / `uf.pages.<name>` 字面量并逐个
解析,拒绝别名、计算下标和动态遍历。新架构下这个 pass 不用改,只是它对照的那张表从 C++
的 catalog 换成第二层读进来的项目文件。

**第二层构造时校验。** `Page.new{...}` 拒绝畸形表。不算编译期,但确定性、早、消息能说
人话。

## 七、证伪矩阵

矩阵是今天唯一真正抓到过东西的守卫,不能丢。它拆得很干净:

- **分数留第一层。** 每个(模板, 屏)对的 SAD 分数是图像运算,是原语。
- **判分归第二层。** 期望命中、misfire、薄,这些是解释,是策略。三态期望
  (`Match` / `Absent` / `Unclaimed`,见 `d489979`)也是。

第二层因此要提供一个批量匹配原语的调用方式,否则一次矩阵要跨很多次边界。这一点待定,见
第十节。

顺带记一个实测代价:八个元素、不给搜索区域时 `check` 十分钟跑不完;给了搜索区域之后
17.3 秒跑完全部。搜索区域不是优化,是可用性前提。

## 八、项目文件格式

第二层定义,第二层读写,格式里带第二层管的 schema 版本。

**项目字段有专属位置。** 项目今天加一个 `foo`,第二层明年也加一个 `foo`,会静默撞车。
项目字段放在一个保留子表下:

```toml
[element.extra]
my_grid_stride = 5
```

第二层拥有所有非 `extra` 的键,加新键永远不会踩到项目。

**第二层重写文件时必须原样保留它不认识的键。** 否则新版本写的文件被旧版本打开保存一次,
新字段就没了,而且丢的时候不报错。

## 九、会下岗的东西

诚实清单,免得有人以为这些还在。

- `modules/annotation` 的模型层:`Element`、`PageSignature`、`PageReference`、
  `ElementCapabilities`、`ExercisedCapabilities`、`Appearance`、`Holding`、
  `SignatureRole`。设计结论有效,实现位置变了。
- `modules/task` 的能力面 `CapabilitySurface`,以及它给 `uf.elements` 建表的那部分。
- `entry/workbench` 的 `preview.*` 里矩阵的判分部分,以及 `entry/authoring` 里
  `page` / `element` 那组绘制动词。
- `engine` 的 `resolvePage` 和 `findAction`。

活下来的:三个原语、票据账本、SAD、OCR、trace、controller,以及 2026-07-30 到 07-31
两轮标注量出来的所有事实。掩码判据、颜色键的选法、overlay 那组数字是知识不是代码,换个
模型照样成立,`docs/pitfalls/colour-key-annotation.md` 不需要跟着改。

## 十、未定项

**页面图的形状。** 讨论中提出但未定:边至少有三种触发方式(点元素、按键、自发跳转),
而且盖上去和走过去是两种边——浮层关掉要回到原处,那是栈不是边。这决定了第二层的 page
模型长什么样,是下一轮讨论的题目。

> **已裁决(2026-08-01,开发者)。** 边是项目文件里的数据:
> `edge{from, to 集合, via = click(引用)|key|spontaneous, kind = navigate|push|pop}`;
> 页面加 `overlay` / `interrupt` 两个标志,弹窗走全局名单不画 N 条入边;pop 不写死
> 目的地,验证目标是运行时页面栈的栈顶。栈住第二层,带显式深度上限当护栏(防脚本
> 无限 push),**不做**每页层级声明。栈的纪律:栈是信念,观察是真相——凡等到非浮层
> 底座页,栈无条件重置为它(实例:战斗中开着卡牌详情,战斗打完自发跳结算,浮层连
> 底座一起被吞)。框架动词只做 `walk_edge`(执行触发、盯目的地集合、超时先查
> interrupt 名单),不做寻路。事实/策略刀法:边通向哪里是事实,走不走是策略。
> 第一块试金石:放大地图(教学第 13 步「点缩小按钮返回上一层」= push/pop)。

**确认与识别的两个动词。** 期望某一页时只验一个标记就够,还是需要更多。倾向由工具挑而
不是作者选,因为矩阵本来就有每个标记对每块屏的全量数据,知道哪个区分力最强。未定。

**确认失败之后。** 讨论中提到先查一小撮声明过的打断页(弹窗、错误框),再退化到全量识别,
也可以用 OCR 读文字判断。打断页要不要是一个显式的页面种类,未定。

**多实例元素。** 滚动网格里同一个元素有 N 个实例,位置随滚动变。不挡页面图,但决定
element 的粒度,未定。

**批量匹配的边界代价。** 见第七节。矩阵和一次页面确认的跨边界次数是否需要一个批量原语,
待测量后定,不要先猜。

## 十一、要修的既有文档

- `2026-07-29-three-layer-task-system.md`:第三节第一层清单、依赖方向图、第四节的
  `cycle_page` / `cycle_find`。
- `2026-07-31-annotation-model-capabilities.md`:加日期注,说明模型设计有效但归属变更。
- `docs/ARCHITECTURE.md`:模块边界与依赖图,`engine -> annotation` 那条。
- `CONTEXT.md`:element 与 page 现在是第二层概念,不是 C++ 类型。
- `docs/knowledge/{en,cn}/module-annotation.md`:两个镜像一起改。

代码一行未动。本文 review 通过之后才开工。

## 十二、2026-08-01 融合裁决

同[目标形态 — 三层系统与 Agent 操作者](2026-08-01-three-layers-and-agent-operator.md)
对齐时开发者当场裁的三条,记在这里,免得两份文档各说各的。

**一、状态读出用有名字的 appearance。** 同一矩形上的互斥状态是**一个**元素带一张命名
appearance 列表,命中身份上到脚本面(`hit.appearance == uf.appearances.speed_3x`);
同日一度提过的替代方案——把 `speed_1x` / `speed_2x` / `speed_3x` 拆成各自独立的元素
——**否决**。沿用的是
[标注模型文档](2026-07-31-annotation-model-capabilities.md) §四之二.4(a),那一节的
定义边界与「页面决定的形态在引用侧钉死、每周期只搜一次」一并有效。术语统一写
appearance(曾称 variant)。

**二、期待文字是数据,核对是策略。** 期待文字作为**数据**写进项目文件,所以它可证伪、
可重放;「怎么算匹配」(全角半角、繁简、包含还是相等)的核对逻辑归第二层 Luau;
第一层的 `cycle_read` **不收** `expected` 参数。第 4 节那张能力面表因此不动。

**三、§五 那条保证线获确认,并点名了裸点击的消费者。** C++ 的硬保证收敛为四条:hit
来自**本票据**的 `cycle_match`(同帧)、观察租约新鲜度、指纹兼容、票据单次消费。
「业务脚本只点标注元素」不消失,只是执法者变成**可信第二层**:裸点击原语以闭包
upvalue 只交给第二层,业务环境暴露的是「点元素 / 点容器内选中项」这类包装,裸点击不在
那张表上;探索环境(Agent 前端)才装载裸点击。原来四要件里「已解析页面授权该元素」
那一项随 element / page 模型一起上移第二层。

另外两条对照:

- **顶层形态**——Agent 是第三种操作者、探索/运行两种信任模式、教学与沉淀协议、动词
  全集(含滚轮、拖拽、同类枚举、语义等待)——见
  [三层系统与 Agent 操作者](2026-08-01-three-layers-and-agent-operator.md)。本文管
  物理归属,那份管能力面与形态。
- **第十节「多实例元素」的方向已经有了**:就是该文档 §七 的**同类枚举**动词(区域内
  某类元素的全部命中,返回「类型 + 位置」清单)。粒度问题仍未定,但不再是空白。
