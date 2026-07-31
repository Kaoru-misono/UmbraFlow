# 架构知识库

本目录记录 UmbraFlow 当前的可执行架构，供开发者查找模块职责、关键入口和测试。
模块依赖以 [`docs/ARCHITECTURE.md`](../../ARCHITECTURE.md) 为准；已经确定但尚未完成的工作见
[`docs/plans/`](../../plans/README.md)。

## 推荐阅读顺序

1. [`00-overview.md`](00-overview.md) — 先看系统地图和主运行路径。
2. [`module-core.md`](module-core.md) — 错误、数值安全、所有权和基础类型。
3. [`module-domain.md`](module-domain.md) — 帧身份、坐标空间、目标代际、检测和观察租约。
4. [`module-vision-image.md`](module-vision-image.md) — Gray8/SAD 识别、PNG、像素布局和模板资产。
5. [`module-annotation.md`](module-annotation.md) — 编辑文档、运行时清单、页面解析和动作授权。
   **自 2026-07-31 起 DIRTY**——标注模型已变成能力模型，先读它的 banner。
6. [`module-engine.md`](module-engine.md) — 发布物加载、同帧决策、端口编排、动作执行和追踪记录。
7. [`module-script.md`](module-script.md) — Luau 底座：沙箱、预算、interrupt 取消，以及
   framework/project 双环境拆分。
8. [`module-controller.md`](module-controller.md) — WGC、目标连续性、DPI 和严格后台输入。
9. [`entry-workbench.md`](entry-workbench.md) — 标注后端：编辑、预览/证伪、编译和发布。
   **自 2026-07-31 起 DIRTY**——它写的那个 GUI 已归档，标注模型也变了，先读它的 banner。
10. [`entry-cli.md`](entry-cli.md) — 参数解析、离线加载、Windows 适配和退出码。
11. [`entry-input-agent.md`](entry-input-agent.md) — `umbra-input-agent`，标注前端：队列协议、
    cursor、路径围栏，以及 drive/annotation 拆分。
12. [`entry-m0-demo.md`](entry-m0-demo.md) — 已冻结的真机验收程序，以及它和产品代码的边界。

## 待补的页（2026-07-29；2026-07-31 又加了一页）

`modules/task` 与 `modules/trace` 至今没有自己的页，而 2026-07-29 的阶段 3 把任务
policy 整个搬进这两层之后，engine 页与 CLI 页已经在替它们解释一些不属于自己的东西。

> **又欠一页（2026-07-31，`b57b67b` + `f768e6c`）**：**`entry-authoring.md`**。本节标题
> 原本写「待补的两页」，那是 `umbra-workbench` GUI 还是标注入口、`entry-workbench.md`
> 还在写它的时候。GUI 已归档，`umbra-authoring` 成了标注项目的唯一途径，却没有任何一页
> 写它——`entry-cli.md` 讲的是 `umbra-flow` 的 `run`/`drive`，只顺带提过一次
> `umbra-authoring`；`entry-workbench.md` 写的只是 CLI 链接的那个后端库，而且整篇挂着
> DIRTY banner。建议范围：`project` / `page` / `element` / `match` / `check` 的动词面
> （`entry/authoring/command.hpp`、`command-runner.cpp`）；为什么每一次写入仍然经过
> `AuthoringDocument`；以及证伪矩阵（`entry/workbench/preview.*` 里同步跑的
> `runModelCheck`）按[能力模型计划](../../plans/2026-07-31-annotation-model-capabilities.md)
> §四之二.1 仍然欠一个 CLI 动词。

> **时点更新（2026-07-29，`1fb41a7`）**：这里原本写「等阶段 3d 落地后一并写」。
> **阶段 3 已整体完成**（3d `4030ffd` 语义事件 + 校验状态机、3e/3f `1fb41a7` framework
> 单测与一票否决第 6 条），条件满足。建议**现在写，不等阶段 4 的真机验收**：阶段 3 完成
> 意味着这两层的表面已经稳定——十二个原语到齐、`umbraflow-trace/v2` 的事件族固定、
> 校验状态机落在 `modules/trace/source/trace/stream-validator.{hpp,cpp}`——而阶段 4 是
> **用**这个表面写第一个真日常并标定常数，改的是数值不是形状。等阶段 4 只会让这两页在最
> 需要它们的那一刻（照着一条 trace 读一次真机失败）刚好还不存在。

> **数目更正（2026-07-30，`ed38124`）**：私有表上现在是**十三个**原语而不是十二个——
> `key` 加在 `cycle_click` 旁边。上面那段照原样保留：它当时的主张是「表面已经稳定」，
> 这一点仍然成立；变的是来了第二个前端，而它需要一次按键，不是这一层的形状动了。
> 下面的范围随之扩大，两页仍然欠着。

建议范围如下：

- **`module-task.md`** — `TaskHost` 的 D10 动词形与 run 生命周期；私有能力面的两道
  接缝（`installer()` 的数据表 / `privateCapabilities()` 的原语表）与它为什么私有；
  观察周期协议与 `CycleLedger`，含 `consume` 旁边的 `spend`——它是一个独立方法而不是一个
  标志位，好让每个调用点自己说出「我这个输入有没有坐标要授权」；Tier A/B/C 的载体形状；
  受信任 Luau framework（`runtime/ctx.luau` + `runtime/task.luau`）承担哪些 policy，
  以及「C++ 拥有保证、Luau 拥有 policy」这条边界线具体划在哪。
  **2026-07-30 起还必须写清第二个前端**：`task::OperatorSession` 是同一批私有原语的同级
  消费者而不是通往 Luau 的路、把两者变成互斥的那道 per-generation 前端闩，以及 `key` 原语
  ——Luau 侧是 `ctx:key(ticket, name)` 与 `view:key(name)`，要求一个打开的周期并花掉它，
  没有命中序数、也没有 page 要求。**不重复** `module-script.md` 已经写清的沙箱、
  预算与双环境机制，也不重复 `entry-cli.md` 已经写清的操作者线协议。
- **`module-trace.md`** — `umbraflow-trace/v2` 的 schema 所有权、事件族
  （`run.*` / `engine.*`（含 `engine.key_delivered`）/ `task.native_call`，以及 3d 起的八条
  `framework.*`）、字段顺序与 golden 比较的规则、非 golden 字段集、`ITraceSink` 的同步可失败
  契约与失败优先级、`frontEnd` 这个盖章字段（`"task"` / `"operator"` / `"annotation"`，最后一个
  没有 run 也没有 generation，因此根本到不了 trace 行——见 `entry-input-agent.md`；它属于盖章而不
  属于事件本身，而且同时是一条协议规则——校验器在除 task 之外的任何流上拒绝 `framework.*`）、
  `frontEndWireName` 作为这个闭集的唯一拼写，
  以及「审计日志而非重放日志」这条定位。**3d 之后还必须写清校验状态机**：
  `TraceStreamValidator` 为什么由 `TraceRecorder` 持有（recorder 是全仓唯一通向 sink 的
  路径，所以绕不过去）、两个失败 kind 的分界（Tier B `InvalidResource` 是 project 造成的
  请求被拒、`InternalInvariant` 是协议破坏并花掉 generation）、以及 step 作用域是**盖章**
  而不是检查——由此 step path **不是**一个 run 内的唯一地址，区分重复的是 `retry_attempt`。
