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
6. [`module-engine.md`](module-engine.md) — 发布物加载、同帧决策、端口编排、动作执行和追踪记录。
7. [`module-script.md`](module-script.md) — Luau 底座：沙箱、预算、interrupt 取消，以及
   framework/project 双环境拆分。
8. [`module-controller.md`](module-controller.md) — WGC、目标连续性、DPI 和严格后台输入。
9. [`entry-workbench.md`](entry-workbench.md) — GUI 编辑、采集、预览、编译和发布。
10. [`entry-cli.md`](entry-cli.md) — 参数解析、离线加载、Windows 适配和退出码。
11. [`entry-m0-demo.md`](entry-m0-demo.md) — 已冻结的真机验收程序，以及它和产品代码的边界。

## 待补的两页（2026-07-29）

`modules/task` 与 `modules/trace` 至今没有自己的页，而 2026-07-29 的阶段 3 把任务
policy 整个搬进这两层之后，engine 页与 CLI 页已经在替它们解释一些不属于自己的东西。
建议范围如下，等阶段 3d（语义事件 + 校验状态机）落地后一并写，免得写完立刻过期：

- **`module-task.md`** — `TaskHost` 的 D10 动词形与 run 生命周期；私有能力面的两道
  接缝（`installer()` 的数据表 / `privateCapabilities()` 的原语表）与它为什么私有；
  观察周期协议与 `CycleLedger`；Tier A/B/C 的载体形状；受信任 Luau framework
  （`runtime/ctx.luau` + `runtime/task.luau`）承担哪些 policy，以及「C++ 拥有保证、
  Luau 拥有 policy」这条边界线具体划在哪。**不重复** `module-script.md` 已经写清的沙箱、
  预算与双环境机制。
- **`module-trace.md`** — `umbraflow-trace/v1` 的 schema 所有权、事件族
  （`run.*` / `engine.*` / `task.native_call`，以及 3d 之后的 `framework.*`）、
  字段顺序与 golden 比较的规则、非 golden 字段集、`ITraceSink` 的同步可失败契约与
  失败优先级，以及「审计日志而非重放日志」这条定位。
