# Lua 任务模型 — 待敲定设计（grill 议程）

> 状态：设计草案，等开发者 grill。这是产品重定义阶段的承重墙——
> 让 UmbraFlow 从"能跑的原语"变成"不重编译就能写任务的框架"。
> 技术底座已定：Lua 5.4 + sol2；trace=JSONL、诊断=借 April2 logger、
> 统一 sink 分发（见 memory umbraflow-logging-decision）。
>
> 每一条给出【我的草案立场】+【为什么有争议】，供开发者挑战。
> 灵魂约束（不可退让）：确定性、可追踪、严格后台、核心零游戏分支。

---

## Q1. `observe()` 的语义：一次调用一帧，还是"当前帧"多识别器查询？

**我的草案**：`observe(recognizer)` 每次都抓一张新帧再匹配。但单帧 8 MiB，
若一个决策要查多个识别器（"是 Home 还是 Result 还是弹窗？"），每识别器重抓
太浪费。倾向：`local frame = bot:capture()` 显式取一帧 →
`frame:find(home)` / `frame:find(reset)` 对同一帧查多次；下一次
`bot:capture()` 才推进。

**为什么有争议**：这把"一次观察一个坐标动作"（DESIGN §8.3 的铁律）从
结构强制变成了约定——脚本可能拿旧 frame 去 act。租约（Q2）必须兜住。

---

## Q2. 命令式 Lua 里怎么保住"不对失效观察下手"？（灵魂）

**我的草案**：`find()` 返回的 Detection 带一个租约（绑定 frame 的
generation + 时间）。`bot:click(detection)` 在投递前重校验租约——若期间
来了新帧、generation 变了、或超时，click **失败**（返回 err / raise），
脚本必须重新 capture+find。于是"确定性"从状态机的**加载期证明**（可达性/
终止性）退化为**运行期契约 + 全量 trace**。

**为什么有争议**：加载期能证明的东西（这个任务一定会终止吗？所有 goto 可达
吗？）在命令式 Lua 里证明不了——写错的脚本要到运行时才炸。这是换 Lua 的
根本代价，值不值、够不够，要你拍。

---

## Q3. 识别器/模板在哪声明：Lua 里，还是 manifest/项目包？

**我的草案**：模板 PNG + ROI + 阈值放**项目包的 manifest**（TOML/JSONC），
Lua 里按名字引用：`bot.templates.home`。资产与代码分离，也正好喂给标注工具
（截图→框选→写进 manifest）。

**为什么有争议**：另一种是全在 Lua 里声明（`local home = template("home.png",
roi)`），更少文件但资产混进代码、标注工具难对接。也关系到"核心零游戏分支"——
游戏专用的东西全进项目包，不进框架。

---

## Q4. 错误模型：raise（pcall）还是返回 nil/false？

**我的草案**：可恢复、脚本该处理的（超时内没匹配到）→ 返回 nil/false；
硬失败（目标断开、guard 违规、租约失效）→ raise Lua error，宿主 catch、
写 trace、判该次 run 失败。对应 Rust 的 Result vs panic 分野。

**为什么有争议**：raise 让脚本更简洁（不用每行判错）但控制流被异常打断；
全返回值则啰嗦但显式。混用要划清哪类走哪条。

---

## Q5. 取消/暂停/超时怎么跨协程工作？

**我的草案**：任务主体跑在 Lua 协程里，每个 `capture/wait/click` 内部
yield。宿主在 resume 之间检查停止标志（Ctrl-C）、暂停请求、预算/超时；
取消时停止 resume → 补发未释放的 Up → flush trace。脚本作者**完全不写**
取消代码，透明。协程 yield 点天然就是 §9.2 要的取消检查点。

**为什么有争议**：sol2 的协程封装边界、以及"暂停期间预算冻结、恢复作废
观察"这些 §9.2 语义能否干净映射到 yield/resume，要设计验证。

---

## Q6. 随机弹窗（interrupts）怎么声明？

**我的草案**：`bot:on(popup_recognizer, function(d) bot:click(d) end)`，
宿主在每个观察周期前先检查已注册的 interrupt 识别器，命中就跑 handler
再回主流程（对应 DESIGN ADR-013 的任务级 interrupts + max_hits）。

**为什么有争议**：命中频率、max_hits、handler 里能不能再 observe/act、
handler 与主流程的 trace 如何区分——都要定。

---

## Q7. 子任务/复用：Lua 函数互调，还是宿主级任务注册表？

**我的草案**：就用 Lua 函数——一个任务是个函数，可以 `require` 或调用别的
Lua 函数做子流程。不做宿主级的 CallTask/Return 机制（那是状态机时代的东西）。

**为什么有争议**：变量作用域、子流程的 trace 归属、以及"能不能被独立测试"
都随之改变。

---

## Q8. 能力/兼容性门（分辨率、目标）在哪把关？

**我的草案**：任务的 manifest 声明要求（基准分辨率、需要的模板、能力）；
宿主在 run 前对目标/分辨率做 fail-closed 校验，不匹配就启动失败（对应
DESIGN §6.2 双层协商 + compatibility.toml）。这里是分辨率自适应将来插入的
挂点。

**为什么有争议**：现在是"不匹配即拒"（ok-script 的教训是这样日常很烦）。
要不要现在就留自适应的口子，还是先严格、后加模块？

---

## Q9. 沙箱：暴露哪些 Lua 标准库？

**我的草案**：锁死——脚本只拿 `bot` 句柄 + 纯 Lua 标准库（string/math/
table）。禁 `io`/`os`/`package`/`require`/`loadfile`/`load`。碰世界的
唯一通道就是 `bot`，这样确定性和 trace 才守得住，任务干不了不可追踪的事。

**为什么有争议**：`require` 禁了，Q7 的子任务复用怎么做？可能要给一个
受控的 `bot:load_subtask("name")`。暴露面越窄越安全，但也越不方便。

---

## Q10. 调度：确实只做"一次一个任务"、无 daemon？

**我的草案**：沿用 DESIGN §9.4——CLI 一次跑一个任务、跑完退出，定时靠
Windows 计划任务。不做队列/优先级/常驻。GUI 调试窗和浮层是**只读消费者**，
不改这个模型。

**为什么有争议**：将来产品化想要"托盘常驻 + 任务列表点选启停"，那需要引擎
常驻——是现在就为它留 Engine API 边界，还是坚持一次一进程、以后再说。

---

## 需要开发者补充的（我无法自己知道的）

- **M0 场景到底是什么**：DESIGN §26 的"Home→点击→等 Result→点击 Reset→
  等 Home"在卡厄思梦境里对应哪个游戏内流程？（真机截图显示游戏当前在角色
  详情页，不是这个场景的起点。）
- 第一条真实日常任务想自动化的是什么（决定 API 够不够表达）。
- 分辨率自适应的优先级：现在做，还是先严格拒、后补模块。
