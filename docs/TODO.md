# UmbraFlow C++ — 当前执行清单

> 状态基线:2026-07-21。产品方向与阶段退出标准以
> [`2026-07-21-product-form-and-roadmap.md`](plans/2026-07-21-product-form-and-roadmap.md) 为唯一权威;
> Luau 任务语义以
> [`2026-07-21-lua-task-model-grill-decisions.md`](plans/2026-07-21-lua-task-model-grill-decisions.md) 为实现层存款。
> 本文件只记录执行顺序,不重复维护产品裁决。
>
> **执行按 A/B 穿插薄片**(S0 共享地基 → A1/B1/A2/B2/A3/B3,顺序不固定;详见 Roadmap 交付顺序):
> 下面 §1/§2/§3 按 P0-A/B/C 归类**能力**,不代表严格先后。**标注整体设计(schema/ROI 坐标空间/page 语义)
> 另出专门设计稿,当前待定**,S0 未落锤前不展开标注实现。

## 0. 现有底座与真机收尾

- [x] Rust→C++ 移植:domain / vision / controller / m0-demo。
- [x] WGC 真机截图:卡厄斯梦境 1600×900 客户区已验证。
- [x] 完成提权 input-agent 的后台点击 before/after 验收(2026-07-21 通过):头像切换 ×3、
      标签切换 ×3、模态识别+安全关闭;严格后台 PostMessage 投递、K2 delta=0;真机首次触发
      租约 fail-closed(StaleObservation)。发现 WGC 静态页 stall,记入
      [`2026-07-20-post-port-win32-robustness.md`](plans/2026-07-20-post-port-win32-robustness.md)。
- [ ] 在 P0-C 前补遮挡、最小化/CaptureStalled、投递中 Ctrl-C 与 10–20 分钟长程验证。

## 1. P0-A — 可视化标注系统

- [ ] 落锤 manifest/annotation schema、ROI 坐标语义、page signature 组合规则与 authoring UI 技术栈。
- [ ] 独立 GUI:WGC 抓帧/导入图片、样本列表、画布缩放/平移、框选编辑、undo/redo。
- [ ] 标注类型:`page_anchor`、`action_target`、`info_region`;属性面板编辑 page、识别方式、阈值及
      required/forbidden 关系。
- [ ] 一键生成/更新模板、manifest 与 page signature,无需手改配置。
- [ ] 使用 runtime 同一识别器 Preview/Test,显示命中框、confidence、Unknown/Ambiguous 原因。
- [ ] 建立卡厄斯梦境关键页面的正例、负例和易混淆静态截图回归集。

## 2. P0-B — Luau Engine

- [ ] 固定 Luau 精确版本,接入 compiler/VM 与 `IScriptRuntime` 可序列化边界。
- [ ] 最小 capability API 与 observe/act/wait 引擎循环;manifest 只读 recognizer/page 句柄。
- [ ] 每任务 VM generation、allocator 配额、interrupt 硬取消、逻辑时钟/RNG 与 generation 热加载。
- [ ] Fake Controller 帧序列、结构化 trace、资源快照和静态截图回归接入 CI。
- [ ] 能力/兼容性门、恒等 `CoordinateTransform`、租约校验与动作后强制作废观察。
- [ ] 通过 Roadmap 第五节的 6 条 Luau 一票否决验证。

## 3. P0-C — 卡厄斯梦境完整每日

- [ ] 分解签到、每日面板、逐项前往、战斗、等结算、领奖、回主界面的真实流程与接管起点。
- [ ] 用 P0-A 制作全部页面、控件和信息区域资产;文字读取确实阻塞时才提前引入 OCR。
- [ ] 用 Luau 写完整每日;P0 用**最小 D6 弹窗清扫**(观察周期边界 + 每个长 `wait` 内)+ 同文件复制,
      不建 D6 重机制 / D7 跨文件复用(留 P1)。
- [ ] 全程后台、不抢焦点;Unknown/Ambiguous/StaleObservation 均 fail-closed 并留下可诊断 trace。
- [ ] 整套每日连续稳定跑完一轮,Ctrl-C 500ms 内退出,单轮 10–20 分钟。

## 4. P1–P3 后续

- [ ] P1:弹窗 interrupt、跨文件子任务、均匀缩放自适应、按需 OCR、标注批量维护与 confusion 诊断。
- [ ] P2:托盘 App、项目/任务入口、运行时浮层、HTML trace 报告、计划任务、portable/installer。
- [ ] P3:第二个游戏验证核心零游戏分支。

## 延迟的健壮性台账

- [`2026-07-20-post-port-win32-robustness.md`](plans/2026-07-20-post-port-win32-robustness.md)
  —— HWND 复用竞态、best-effort Up、DPI 与 capture 取消边界。
- [`2026-07-20-m0-demo-port-deviations.md`](plans/2026-07-20-m0-demo-port-deviations.md)
  —— 移植期有意偏差与后续清理项。
