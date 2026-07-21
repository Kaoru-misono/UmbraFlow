# Luau 任务模型 — grill 裁决日志

> 逐题记录开发者与 AI 在 grill 中敲定的**最终结论**(区别于决策包里的"建议")。
> 决策弹药见 [`2026-07-21-lua-task-model-decision-package.md`](2026-07-21-lua-task-model-decision-package.md)。
> 议程原件见 [`2026-07-20-lua-task-model-grill.md`](2026-07-20-lua-task-model-grill.md)。
> 状态:进行中。开始于 2026-07-21。

---

## ⚑ 方向转向(2026-07-21,最重要)

grill 进行到 Q10 时,开发者澄清了真实意图,**修正了整个 grill 的方向**:

- **意图**:Rust→C++ 移植已基本收尾,开发者要**脱离原 DESIGN.md 的约束**,从手头信息 + 调研**从零锚定产品形态与方向**。
- **此前 grill 的偏差**:AI 把旧 DESIGN.md 当权威约束,自下而上 grill 实现细节(租约/generation/沙箱),甚至用 §9.4/ADR-012 反对开发者的形态选择——方向拧了。
- **纠正后的顺序**:**产品最终形态 → 大体方向/差异化 → roadmap → (实现细节最后回填)**。

### 两个已定的锚

1. **产品最终形态 = 托盘常驻个人 App**。写 Luau 任务 → 托盘/GUI 点选启停 → 实时浮层看识别框 → trace 报告可回放 → 定时跑。核心卖点 = **严格后台挂机**(用电脑同时替你挂机,永不抢焦点/不全局注入,按键精灵/AHK/SikuliX 做不干净的护城河)。determinism/trace 是**无人值守挂机的信任地基**,非学术洁癖。
2. **近期目标 = 单游戏做深**。先把开发者自己常玩的那一个游戏做到真好用(完整日常闭环),再谈跨游戏通用性。旧 DESIGN 的"M0 鸣潮→M2 卡厄斯→NIKKE 铺广"被取代。

### D0–D10 的地位重定

下面 D0–D10 是**实现层的存款**,不是产品方向。它们:
- 在旧 DESIGN 约束下敲定,脱离后**部分会变**(Q10/D10 已因常驻 App 形态而变);
- 等新 roadmap 定了、真正进入某阶段实现时**按需取用/复核**,不当作既成约束。
- **收尾头号事项**:取得开发者的新版 DESIGN(含 ADR-013),diff 旧版(897 行 /e/github/UmbraFlow/DESIGN.md),复核受影响裁决。

---

## ⚑ 脚本底座已定 = Luau(2026-07-21,取代 Lua 5.4 + sol2)

开发者调研后**选定 Luau**(取代基线 Lua 5.4 + sol2)。完整裁决(判据对比、落地约束、6 条一票否决验证、C# worker 重评触发条件)见
[`2026-07-21-product-form-and-roadmap.md`](2026-07-21-product-form-and-roadmap.md) 第五节。对 D0–D10 的传导:

- **与语言无关、原样保留的存款**:D0(双 generation)、D1(帧模型 Model B + 动作后失效)、D2(租约 generation 主判据 + max_action_frame_age 兜底)、D3(标注资产/页面 manifest + 只读句柄)、D6(interrupt 同步/周期边界/first-match)、D8(CoordinateTransform 缝 + P0 恒等)。这些是任务语义,不随语言变。
- **被 Luau 原生能力取代实现手段的**(结论不变,机制换):
  - **D5 取消**:`lua_sethook` 指令 hook → **Luau interrupt callback(safepoint)**;死循环强停靠 interrupt 检测取消后 yield、宿主放弃旧线程(官方 Conformance 测试覆盖)。双预算(指令/时间)概念保留。
  - **D9 沙箱**:空 `_ENV` + 手工禁库 → **Luau 原生 sandbox 原语 + 递归 readonly 宿主 API 表**;只接收源码经受控 compiler 生成 bytecode、不收磁盘/网络/用户 bytecode;确定性由宿主协议保证(注入逻辑时钟 + 固定算法 RNG,禁 dict 遍历顺序决策)。
  - **D4 错误 Tier C**:"取消不可被 pcall 吞" → Luau 落地为**取消最终信号不用可被 pcall 捕获的普通脚本错误**,而是 interrupt yield/abandon;Tier A/B 的返回值 vs 抛异常分野不变。
  - **D7 子任务**:`package.preload` → Luau 受控多脚本加载(同沙箱环境、内容哈希寻址,概念不变)。
- **作废的 sol2/Lua5.4 专属细节**:`lua_sethook`、`sol::`、`SOL_EXCEPTIONS_SAFE_PROPAGATION`、`load_mode::text` 等具体 API 名 —— 进入实现时映射到 Luau/对应 C++ 绑定后**重新验证**,不作为约束。

---

## D0. 双 generation 计数器语义(地基,critique #1)

**结论(已定)**:保留两个正交计数器,注入层投递坐标动作前**两个都校验**,任一不符 → fail-closed 抛 `StaleObservation`,绝不静默投递。

- **FrameId**:每次 `capture()` 自增(高频),回答"是不是同一张截图 / 帧是否新鲜"。承载 liveness 维度。
- **TargetGeneration**:仅由窗口层事件(目标窗口重建、HWND 复用、分辨率变更、过能力门 / resume 重新校准)自增(低频),回答"操作的还是不是原来那个游戏窗口 / 目标同一性"。承载 safety 主判据。
- ObservationLease 两字段各管一维;Controller fencing 同时校验 `frame_id` 与 `target_generation`。

**待后续题继续钉的关联点**:"任一坐标动作是否也 bump FrameId、令当前帧强制失效"(§8.3 从约定变强制)—— 见 D1。

---

## D1. Q1 帧模型:observe() 语义

**结论(已定)**:采纳 **Model B + 动作后整帧失效**。

- 脚本显式 `local frame = bot:capture()` 取一帧,对**同一帧**多次 `frame:find(recognizer)`(同帧多识别器查询,只抓一次 8MiB 帧)。
- **任一坐标动作(click/swipe/key…)执行后立即 bump FrameId**,令所有已取得的 Frame/Detection 句柄失效;脚本复用动作后的旧 frame/detection → fail-closed 抛 `StaleObservation`,强制重新 `capture()`。
- 效果:§8.3"一次观察→一个动作→必须重新观察"铁律从状态机时代的**结构强制**,转为 Luau 里的**运行时契约 + 注入层 fencing 兜底**;代价是脚本错误运行时才炸(换命令式脚本的根本代价)。

**留待常数标定(归后续统一定)**:`max_action_frame_age` 实际值、是否暴露 raw `find`、是否接受 `capture`+`find` 双名词命名、单次决策实际查几个识别器(依赖第一条真实任务)。

---

## D2. Q2 租约:怎么保住"不对失效观察下手"

**结论(已定,按"避免过度设计"收敛到最简形态)**:

- **generation 比对是唯一主判据**(fail-closed):就是 D0 的两个计数器(FrameId + TargetGeneration),注入层投递前比对,任一不符立即拒。这一维不读墙钟,确定性天然保证。
- **`max_action_frame_age` 一个墙钟上限**(沿用 DESIGN §5.4 既有设计)作"陈旧帧保险丝":防 `capture` 后长时间无动作、界面被游戏自己改变而 generation 未变的情况。墙钟只出现在这个"太老就拒"的兜底路径上,拒绝是安全方向(fail-closed),不影响正常路径可复现性。
- **不引入**"safety/liveness 两维正交术语"或"逻辑 tick 双轨"——那是过度设计,砍掉。
- 租约失效 → **直接抛 `StaleObservation`,不内建重试**(Q2-b);重试留给脚本显式表达或 Q4 的受控 `bot:try`。

**留待**:`max_action_frame_age` 实际值(与 D1 合并标定);StaleObservation 在 Q4 归哪一层错误(→ D4)。

---

## D4. Q4 错误模型:raise 还是返回值

**结论(已定)**:采纳**三层**模型 + trace 抛出瞬间 emit。

- **Tier A — 预期内缺席** → 返回 `nil`。例:`frame:find(home)` 没找到。正常控制流,不抛异常,脚本 `if not d then`。
- **Tier B — 可恢复硬失败** → 抛**可被 `pcall`(或未来 `bot:try`)捕获的结构化错误**。例:`StaleObservation`(D2 的租约失效归此层)、目标暂时拒绝。脚本可捕获重试。
- **Tier C — 宿主控制信号(取消 / 预算耗尽)** → 抛**脚本 `pcall` 抓不到、必穿透**的特殊错误。理由:取消若能被 `pcall(function() while true do end end)` 吞掉,§9.2"500ms 内响应取消"就破了。此层是三层模型的真正必要性所在(A/B 只是常规"返回值 vs 抛异常"分野)。
- **trace 时机**:错误在**抛出瞬间** emit 到 trace,**先于**脚本 pcall。脚本即便 pcall 吞掉 StaleObservation,trace 仍有记录 —— 可追踪性(灵魂约束)不依赖脚本自觉。

**留待**:各 kind 的 `retryable` 默认值;暴露原生 `pcall` 还是受控 `bot:try`;`SOL_EXCEPTIONS_SAFE_PROPAGATION` 配置;wait 超时(Tier B)与预算耗尽(Tier C)拆成不同 kind(critique #3)—— 这些与 Q5 取消/预算一起标定。

---

## D9. Q9 沙箱:信任模型与白名单

**结论(已定)**:锁定**可信作者模型**(当前 P0–P3),沙箱按白名单锁死。

- **信任模型**:当前 P0–P3 假设脚本作者可信(自己/团队写,自动化自己在玩的游戏)。沙箱**唯一目的是守三条灵魂约束**(确定性/可追踪/严格后台),白名单顺带得到相当强的越狱防护,**不为抵御恶意第三方脚本额外建设**。第三方脚本市场(签名/强隔离/细粒度授权)不在当前 Roadmap。
- **沙箱手段(灵魂约束的必然结论,直接采纳)**:
  - 每 run 全新 `lua_State` + 空 `_ENV`(隔离、无跨运行泄漏)。
  - 禁 `io`/`os`/`package`/`require`/`load`/`loadfile`/`debug`/`coroutine`(宿主内部可用 debug/coroutine 做调度,但不暴露给脚本)。
  - `string` 去 `dump`;`math.random` → `bot:random`(种子入 trace);`load_mode::text` 挡 bytecode。
  - 补 `bot:now()`(单调时钟)替代真实系统时钟;有序遍历替代无序 dictionary 遍历(迭代顺序不能参与决策)。
- 解锁下游:Q5(硬中断需锁 debug/coroutine)、Q7(子任务复用需受控加载)、Q3(资产只读句柄)。

**留待(critique #7 确定性地板补全)**:`setmetatable`/`raw*`/`next`/`tonumber` 逐个判定;integer/float 边界规则;`__gc`/地址型 `tostring` 消毒;`bot:random` 是否暴露、超越函数跨平台位级复现要求。

---

## D5. Q5 取消/暂停/超时

**结论(已定)**:协程检查点 + **Luau interrupt callback** 硬中断 + 双预算;**pause 推到 P1**。

- **协作式检查点(常规路径)**:任务体跑在 Luau coroutine,`capture/wait/click` 内部 yield;宿主在 resume 之间检查停止标志/暂停/预算/超时。脚本作者不写取消代码,透明(§9.2 天然映射)。
- **硬中断(死循环兜底)**:Luau interrupt callback 在 VM safepoint 检查原子取消标志与执行预算;命中后 yield,
  宿主 abandon 旧线程且不再 resume。纯 Luau 死循环即使不调用宿主 API 也必须在 500ms 总退出预算内停止,
  且取消不能被 `pcall` 吞掉。
- **双预算(都要,抓不同故障,非过度设计)**:①指令预算(防纯 CPU 死循环,确定性可复现);②时间预算 max_runtime(防"慢但不死",如每步真在等游戏累计超时)。少任一个都有漏网。
- **P0 范围**:只做 **cancel + timeout**(证明能跑通 + 能被 Ctrl-C 干净停下);pause/resume(暂停冻结预算、恢复作废旧观察)**推到 P1**。

**留待标定(与 Q4 一起)**:interrupt 回调频率/执行预算、max_runtime 默认值、Ctrl-C 与 Engine.cancel 是否共路径、per-op 分层超时(per-capture/recognition/retry)的 P0 范围。500ms SLA 已由 Roadmap 定为含纯 Luau 死循环的 P0 硬指标。

---

## D7. Q7 子任务/复用

**结论(已定)**:同文件原生 Luau 函数(P0 就有);跨文件复用推 P1;放宽递归。

- **同文件子流程** = 普通 Luau 函数(`local function ... end` + 调用),作用域/return 全用 Luau 原生,**不做**宿主级 CallTask/Return(状态机时代产物)。P0 即可用。
- **跨文件复用** = 宿主受控 `bot:load_subtask("name")`:只加载 manifest 已声明、按内容哈希寻址的源码,
  经受控 compiler 编译后进入**同一受限环境与 runtime generation**;不开放 `require`、磁盘路径或
  `package.preload`。**推到 P1**(P0 允许同文件复制/普通函数,YAGNI)。
- **递归**:放宽,**允许递归**,不单独加深度守卫;终止性统一由 D5 的指令预算 + max_runtime 兜。§8.5"v1 禁递归"在 Luau 下改为此策略(递归写错与 while 死循环写错同由一个预算兜住,再加守卫是冗余)。

**留待**:子任务输出形态、是否需命名空间/版本(推到 P1 设计 load_subtask 时定)。

---

## D8. Q8 能力/兼容性门 + 分辨率自适应

**结论(已定)**:双层门在 state 创建前;坐标变换留完整缝 + P0 恒等;**自适应提到 P1**。

- **能力门**:双层门(Backend Capability + Target Compatibility,§6.2)在 **Luau VM state 创建之前** fail-closed 校验,不匹配根本不进脚本执行。灵魂约束必然结论,无争议。
- **坐标变换缝**:内部所有"帧坐标 ↔ 目标物理坐标"转换统一走 `CoordinateTransform{scale, offset, viewport}`,**不散落裸标量 scale**;P0 只允许 identity(scale=1/offset=0),不匹配即 fail-closed 拒。零额外成本,未来自适应是"填非恒等值"而非改架构。
- **自适应排序**:**提到 P1**。只做最朴素均匀缩放归一化(按比例缩放模板/ROI),不做锚点复杂布局。

**⚠ 连带信号(收尾归入待确认)**:选"自适应提到 P1"强烈暗示**首任务需要跨分辨率/带 DPI 才能日常用**。这会反向影响 P1 范围。需开发者确认:首任务实际运行分辨率、目标游戏 scaling_mode、DPI 是否需 P1 归一。

**留待**:base 分辨率取值、DPI 归一细节。

---

## D3. Q3 识别器/模板在哪声明

**结论(已定)**:P0-A 可视化标注系统生成项目资产/页面声明;Luau 只拿只读句柄;P0 只允许字面量引用。

- 模板/颜色 recognizer、ROI、阈值、`page_anchor`、`action_target`、`info_region` 与 page signature
  **强制进入项目包声明**,由 P0-A GUI 写入;加载期校验资产存在、引用闭合、ROI 边界和 page 歧义规则。
- Luau 只得到**只读 opaque recognizer/page 句柄**;**不暴露** `template("home.png", roi)` 或任意路径读图等
  脚本内构造能力。确切命名空间随 annotation schema 落锤,但资产与代码分离是硬约束。
- **参数化 ROI(Layer3)在 C++ 侧算**,Luau 不碰坐标运算;**P0 不做 Layer3**(YAGNI)。
- **P0 只允许字面量引用**,加载期 100% 枚举校验;动态索引等真实需要出现后再放宽。

**留待(P0-A 开工前必须落锤)**:manifest/annotation schema、recognizer/page 句柄命名空间、ROI 坐标空间、
required/forbidden 组合与 Unknown/Ambiguous 证据格式(均与 Roadmap P0-A、D8 CoordinateTransform 对齐)。

---

## D6. Q6 随机弹窗 interrupts

**结论(已定)**:采纳"同步 · 周期边界 · 注册顺序 first-match · 禁重入 + max_hits 显式失败"模型;通用 interrupt **推到 P1**。

- `bot:on(popup_recognizer, handler)` 注册;宿主在**每个观察周期边界同步**检查已注册 interrupt(不异步、不在动作中途插入,守 §8.3 与 trace 顺序)。
- 多命中按**注册顺序 first-match**(确定性平局裁决);**handler 期间禁重入**(防无限递归弹窗);同一 interrupt 超 `max_hits` → **显式 raise**(不静默)。
- **P0 不做通用 interrupt**,随机弹窗先用显式 `if`/wait 分支跑通整套每日;P1 再用 `bot:on` 回填抽象。

**⚠ ADR-013 澄清(重大,影响全局)**:开发者确认**手里有比 `/e/github/UmbraFlow/DESIGN.md`(897 行,§24 止于 ADR-012)更新的 DESIGN 版本,其中已含 ADR-013**。因此:
- **不新写 ADR-013**,以开发者新版 DESIGN 为准。
- **⚠ 整个 workflow 与决策包基于旧版 DESIGN.md 跑出**——新版若有实质改动,部分裁决依据需复核。**收尾头号事项**:取得新版 DESIGN,diff 旧版,复核受影响裁决。

**留待**:max_hits / on_exhausted 默认、作用域(run vs step)、handler 能否调子任务、致命弹窗能否中止 run、首任务实际有哪些弹窗。
