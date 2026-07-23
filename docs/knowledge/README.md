# 架构知识库

本目录是 UmbraFlow 当前可执行架构的开发者导航：它以代码现状为准，串联各模块与组合入口的职责、关键类型、数据流、设计不变量、测试策略和扩展接缝，帮助读者从全局目标逐层下钻，并在修改前找到真正拥有某项语义的边界；权威架构和已裁决计划仍分别以 [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md) 与 [`docs/plans/`](../plans/README.md) 为准。

## 推荐阅读顺序

1. [`00-overview.md`](00-overview.md) — 先建立全系统地图，理解从视觉证据到严格后台动作与 JSONL trace 的完整链路。
2. [`module-core.md`](module-core.md) — 从无项目依赖的能力内核开始，掌握错误、数值安全、所有权和基础类型约束。
3. [`module-domain.md`](module-domain.md) — 继续学习帧身份、坐标空间、目标代际、检测与观察租约等共享语义。
4. [`module-vision-image.md`](module-vision-image.md) — 查看确定性 Gray8/SAD 识别核，以及受配额约束的 PNG、像素布局和模板资产链。
5. [`module-annotation.md`](module-annotation.md) — 理解 authoring/runtime 双文档、内容寻址编译、页面解析、识别证据和动作授权。
6. [`module-engine.md`](module-engine.md) — 跟随运行时如何加载发布物、保持同帧决策、编排端口、执行授权动作并记录 trace。
7. [`module-script.md`](module-script.md) — 补充阅读独立的 Luau 嵌入底座及其与未来无人值守 runtime 之间尚未闭合的边界。
8. [`module-controller.md`](module-controller.md) — 下钻唯一的 Windows reusable module，了解 WGC、目标连续性、DPI 与严格后台输入投递。
9. [`entry-workbench.md`](entry-workbench.md) — 从 authoring 入口观察 GUI、采集、Preview、编译和原子发布如何组合。
10. [`entry-cli.md`](entry-cli.md) — 从产品运行入口观察参数、离线加载、Windows adapters、单次动作流程和退出码契约。
11. [`entry-m0-demo.md`](entry-m0-demo.md) — 最后阅读冻结的真机验收底座，区分已验证的 WGC/输入证据与可继续扩展的产品实现。
