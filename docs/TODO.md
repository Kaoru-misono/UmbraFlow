# UmbraFlow C++ — 待办事项

> 状态基线：2026-07-20。Rust→C++ 移植已完成并提交（domain / vision /
> controller / m0-demo，5 个 commit + clangd 修复 + capture/input-agent）。
> 从零构建 8/8 CI 全绿。下面是尚未完成的工作，按优先级排列。

## 1. 真机验收（当前唯一在途，需开发者启动一次提权）

AI 无法自我提权（Claude Code 安全分类器硬拦），所以提权 input-agent 由开发者
手动启动一次，之后 AI 非提权驱动。步骤见
[`docs/plans/2026-07-20-ui-verification-runbook.md`](plans/2026-07-20-ui-verification-runbook.md)。

- [ ] 开发者跑 runbook 里的那条提权启动命令（一次 UAC）。
- [ ] AI 驱动 UI 验证：点头像切角色、点标签切内容，带前后对比图给结论。
- [ ] （可选，源自 Rust U1）guard/coexist 循环、100 连环、遮挡/最小化、
      live WGC stall、投递中 Ctrl-C——这些需要真实模板与场景。

## 2. 产品重定义 — Lua 任务模型（承重墙，下一步）

先设计后编码。grill 议程（12 个待敲定问题 + 我的草案立场）见
[`docs/plans/2026-07-20-lua-task-model-grill.md`](plans/2026-07-20-lua-task-model-grill.md)。

- [ ] grill 敲定 Lua API 面：`observe/act/wait` 形态、命令式脚本里怎么保住
      "不对失效观察下手"（租约 + 全量 trace）、模板放项目包、错误模型、
      协程取消、弹窗 interrupt、沙箱边界。
- [ ] 技术底座：Lua 5.4 + sol2。
- [ ] 日志架构：trace=JSONL、诊断=借 April2 logger、统一 sink 分发
      （见 memory umbraflow-logging-decision）。

## 3. 产品重定义 — 框架层（Rust 从未实现，全新设计）

- [ ] **执行引擎**：把 m0-demo 的固定循环抽成"读 Lua 脚本、驱动
      observe/act 周期"的通用引擎（取消/暂停/超时、变量/子任务）。
- [ ] **Fake Controller**：脚本化帧序列，让 runtime/任务逻辑脱真机可测
      （力量倍增器——把开发者从"唯一真机测试者"的瓶颈里解放）。
- [ ] **trace + 关键帧 + 离线回放**。
- [ ] **实时调试浮层**（游戏窗口上画识别框/状态；WDA_EXCLUDEFROMCAPTURE +
      WS_EX_NOACTIVATE，不违反后台纪律）。
- [ ] **能力/兼容性门**（分辨率/目标 fail-closed 校验）。

## 4. 产品重定义 — 能力模块（可选，日常好用的门槛）

- [ ] **分辨率自适应**（现为不匹配即拒；ok-script 的教训是这样日常很烦）。
- [ ] **OCR**（读数字/文字状态，许多真实任务需要）。
- [ ] **标注工具**（截图→框选→出模板+元数据；capture 模式已是上游第一块砖）。
- [ ] **HTML 运行报告**（把 trace 渲染成可点开的时间线）。
- [ ] **调试 GUI 窗口 / 托盘**（Dear ImGui + D3D11；只读消费 Engine 事件）。

## 5. 延迟的健壮性项（移植期记录，产品阶段回填）

见两份台账：

- [`docs/plans/2026-07-20-post-port-win32-robustness.md`](plans/2026-07-20-post-port-win32-robustness.md)
  —— HWND 复用竞态、best-effort Up 补发、DPI 边界等（均忠实照搬 Rust，
  非移植缺陷）。
- [`docs/plans/2026-07-20-m0-demo-port-deviations.md`](plans/2026-07-20-m0-demo-port-deviations.md)
  —— 消息措辞、病理输入解析、尺寸配额等有意偏差。

## 杂项

- [ ] LICENSE 版权人已填 Kaoru-misono（MIT）。
- [ ] `.claude/settings.json` 的提权权限规则是本地授权，不入版控。
- [ ] Rust 仓库（E:\github\UmbraFlow）已冻结，README 顶部有指向说明。
