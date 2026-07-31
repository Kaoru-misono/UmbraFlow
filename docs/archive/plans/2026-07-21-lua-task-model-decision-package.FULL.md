# UmbraFlow 命令式 Lua 任务模型 — 决策包（grill 前弹药）

> **词汇重定向(2026-07-31)。** 本文是有日期的记录,不改写。下文的
> `recognizer` / `RecognizerId` / `uf.recognizers` / `recognizerId` 一律读作
> **element** / `ElementId` / `uf.elements` / `elementId`;`RecognizerDefinition`
> 与 `RecognizerVariant` 读作 `CompiledElement` 与 `CompiledAppearance`;
> `Variant` / `variant` 读作 `Appearance` / `appearance`。`RecognitionCatalog` 与
> `RecognitionRuntime` 名字不变——它们指的是「识别」这个动作。schema id 随改名一起动了:
> `umbraflow-authoring/v4`、`umbraflow-annotations/v3`、`umbraflow-trace/v2`。
> 权威词汇见 `CONTEXT.md` 的「Annotation model」一节。

> **状态**：这是 **grill 前的决策弹药，不是最终定稿**。本文档由一次多 agent workflow （5×ground 调研 + 10×grill 逐题裁决 + 3×synth 综合 + 1×critique 完整性审查）的结构化产出组装而成，用于支撑开发者对 Q1–Q10 的当面 grill。所有裁决建议均**待开发者拍板**，凡标注为 open / developerInputNeeded / grillTarget 的条目都尚未定稿。
>
> **grill 议程原件**：[`2026-07-20-lua-task-model-grill.md`](2026-07-20-lua-task-model-grill.md)（Q1–Q10 草案立场 + 争议点原文，以及"需要开发者补充"三条）。本决策包是对该议程每一题的弹药扩写。
>
> **四条灵魂约束（不可退让）**：确定性、可追踪、严格后台、核心零游戏分支。
>
> **背景**：产品 UmbraFlow 是通用视觉游戏自动化框架，正把任务模型从"显式状态机"换成命令式 Lua 5.4 + sol2。
>
> **(2026-07-30 补注)** 本档为旧口径留档，正文不改写。其中的 `RecognitionFailed` 已改名为
> `RecognitionIncomplete`（wire 名 `recognition_incomplete`，`FailureResponse` = `Retry`；见
> `modules/domain/source/domain/error.hpp`），且**不要读成"识别失败"**：识别跑完却没匹配上
> 不带任何错误（那是 `UnknownPage` 或空命中），该 kind 只表示比较预算在搜索结束前耗尽、
> 调用方对屏幕一无所知，应重新观察。

## 目录

1. [硬约束底座：换命令式 Lua 丢失了什么、DESIGN 原本靠什么、怎么补偿](#1-硬约束底座)
2. [联网调研要点（四路）](#2-联网调研要点)
3. [Q1–Q10 逐题裁决](#3-q1q10-逐题裁决)
4. [框架设计骨架（synth）](#4-框架设计骨架)
5. [产品能力锚定（synth）](#5-产品能力锚定)
6. [Roadmap 草案（synth）](#6-roadmap-草案)
7. [完整性审查（critique）](#7-完整性审查)
8. [待 grill 解锁清单（按优先级）](#8-待-grill-解锁清单)

---

## 1. 硬约束底座

> 提炼自 ground 的 DESIGN 硬约束调研。核心问题：**换成命令式 Lua 后，原本在加载时可证明的性质（可达性 / 终止性 / 资源引用可枚举）哪些丢失、DESIGN 原本靠什么保证、拿什么补偿。**

### 1.1 加载时可证明性质的净损失与补偿

DESIGN §3.3/§8.6 的"加载时失败原则"是整份设计对"加载时可证明"依赖最密集之处，也是命令式 Lua 冲击最大的地方。以下逐项列出 ground 标注为 **[丢失]** 的性质：

#### [丢失] 可达性:无不可达状态(§8.6、§3.3)

- **DESIGN 原本靠什么 / 现状**：状态机是有限有向图,'无不可达状态'=从 entry 做图遍历即可判定;'状态没有合法退出路径'同理(§8.6、§3.3)。原保证来源:有限、可枚举的状态图。
- **补偿方案 / 启示**：命令式 Lua 控制流任意,可达性不可判定,死代码无法证明。此性质无法用等价机制补回,只能降级为 lint(best-effort 提示),不再是加载时证明。grill 需明确承认这条能力的净损失,并决定 lint 深度。

#### [丢失] 终止性:非终止状态有超时 + max_transitions/max_runtime(§8.2、§8.6、§3.3)

- **DESIGN 原本靠什么 / 现状**：Flow 要求每个非终止状态有 timeout,并有 limits.max_runtime(示例 20m)、limits.max_transitions(示例 500)封顶,结构上保证终止(§8.2、§8.6);成功标准3/5 要求有限时间停止、任意状态有超时保护。
- **补偿方案 / 启示**：Lua while/递归=停机问题,加载期无法证明终止。补偿:强制运行时预算——全局 max_runtime(单调时钟)、基于 Lua debug hook 的指令计数预算(确定性、可打断纯 Lua 死循环)、把 §9.2 的分层超时(Recognition Poll/Action Retry/State Recovery)在绑定层重构为 per-capture/per-recognition/per-retry 超时。终止性从'加载时证明'变为'运行时强制'。

#### [丢失] 资源引用可静态枚举(§8.6、§3.3、§22 M1)

- **DESIGN 原本靠什么 / 现状**：Flow 里模板引用是 case 中的字符串字面量,可在加载期枚举并校验存在;M1 退出条件(§22 line723)直接要求'所有状态和资源引用可在加载时验证'。
- **补偿方案 / 启示**：Lua 可动态计算模板名,无法在加载期完整校验存在性。补偿:要求项目在清单中声明脚本可能使用的资产集合(加载期校验该集合存在)+运行时'asset not found'快速失败;或约束模板引用只能来自字面量查找表。必须重新谈判 M1 该退出条件的措辞。

#### [丢失但补偿更强] background_only 禁用动作静态扫描(§8.6、§3.8、§6.4、ADR-011)

- **DESIGN 原本靠什么 / 现状**：Flow 校验器可扫描 background_only 项目未引用被禁止的动作(§8.6);§3.8/§6.4/ADR-011 要求绝不静默降级到前台/全局注入。
- **补偿方案 / 启示**：Lua 可经计算派发调用动作,静态扫描不完整——这条'加载时证明'丢失。但补偿反而更强:采用'能力即缺席'——根本不把任何前台/全局注入/os.execute 路径绑定进 Lua state。强制点从'扫描已声明动作'变为'危险函数在沙箱里不存在'。运行时绑定面成为唯一保证。

### 1.2 其余硬约束（迁移必须逐条守住）

#### 确定性铁律(§3.1)

- **约束**：同帧+同识别器版本+同参数必须产生相同结果;多规则命中用声明顺序或显式优先级解决,highest_score 置信度相同时退化为声明顺序;时间只用单调时钟(§3.1,呼应 §10.2 事件顺序不依赖墙钟)。
- **对 Lua 迁移的启示**：sol2 沙箱必须消除所有不确定性源:禁用/替换 os.time/os.clock/os.date、math.random(或强制确定性种子)、依赖 pairs() 的非确定迭代顺序(Lua 表 pairs 顺序未定义→只用 ipairs/有序容器)、table 的地址型 tostring。需从单调时钟绑定一个确定性时间 API 给脚本。任何'多 case 求值'的辅助函数必须保留声明顺序作为平局裁决。

#### 可观测性优先 / Trace 事件(§3.2、§10.2)

- **约束**：状态迁移、识别、动作、重试、错误都必须产出结构化事件写入 Trace,而非仅人读日志;事件带单调 seq+elapsed_ns,顺序不依赖墙钟(§3.2、§10.2)。
- **对 Lua 迁移的启示**：每个被绑定的动作/识别调用必须由 sol2 绑定层自动 emit JSONL trace 事件,不能依赖脚本作者自觉——脚本无法'忘记'埋点。Lua 无状态概念,需合成 StateEntered/StateTransition/CaseMatched 的等价物(例如脚本可声明的具名 step/label span,或每次绑定调用自动生成 span)。

#### 加载时失败原则(§3.3、§8.6)——核心受冲击点

- **约束**：必须在启动前发现:引用不存在的状态/图片/变量、状态无合法退出路径、Capability/Compatibility 不满足、参数类型/范围错、资源缺失、无不可达状态、非终止状态有超时、background_only 未引用禁用动作、所需 Backend Capability 已声明(§3.3、§8.6)。
- **对 Lua 迁移的启示**：这是整份文档对'加载时可证明'依赖最密集的地方,也是命令式 Lua 冲击最大的地方。§8.6 的加载时契约必须整体重定义:把可静态判定的项(引用/Schema/声明)保留在加载期,把可达性/终止性/资源存在下沉为运行时预算+快速失败(见下四条)。

#### 一次一坐标动作铁律 + ObservationLease(§8.3、§5.4、§26、ADR-004)

- **约束**：串行执行动作,任一输入动作都会使当前 Frame/Detection 失效;坐标动作携带 ObservationLease{session_id,target_generation,frame_id,expires_at},默认 max_action_frame_age=750ms(项目只能调短);Controller 投递前二次校验 lease/generation/边界/句柄(§8.3、§5.4、ADR-004)。§26 规范循环:观察→动作→作废旧 Detection→重新截图。
- **对 Lua 迁移的启示**：Lua API 必须让 Detection/Frame 句柄对坐标动作'一次性':任一输入动作后,先前取得的 Detection 句柄必须失效,复用则抛 StaleObservation 而非静默点击。不能让脚本缓存一个坐标点两次。API 应引导 capture()→recognize()→click(detection) 后强制重新 capture;lease 由绑定层自动附着,脚本不能手造裸坐标绕过校验。

#### 动作白名单与沙箱(§5.5、§15)

- **约束**：内置语义动作为固定枚举(Click/LongPress/MovePointer/Swipe/KeyPress/KeyDown/KeyUp/InputText/Wait/CaptureArtifact/SetVariable/CallTask/Return/StopTask);Shell 和任意 Command 不是默认动作;Flow DSL 不提供网络/Shell/任意路径文件写(§5.5、§15)。
- **对 Lua 迁移的启示**：sol2 沙箱必须移除 os.execute/io/package/dofile/loadfile/require/os.getenv/网络;只暴露上述副作用动作+识别+截图+变量/日志。SetVariable/CallTask/Return/StopTask 应退化为 Lua 原生(local/函数调用/return/error),不再是绑定动作。文件写入只允许经 CaptureArtifact 写入 Engine 配置的运行目录。未来新增任一高风险动作,必须在同一里程碑同时加 project.toml 权限字段+加载期拒绝+测试(§15)。

#### Frame/Detection 不可变与生命周期(§3.6、§5.2、§5.3、§5.4)

- **约束**：Frame 创建后不可改、Arc 共享、缓存有上限;Detection 只在当前观察周期有效,跨状态只提取文本/数值/业务标识不保存 Rect;TargetGeneration 变化使旧 Frame/Detection 失效(§3.6、§5.2-5.4)。
- **对 Lua 迁移的启示**：Frame/Detection 的 sol2 userdata 必须只读,Lua 不能改像素或矩形;Detection 暴露为短命句柄。脚本要跨 step 保留信息只能提取标量(text/number/label),绝不能保留 Rect/句柄。把 Detection 句柄存入长生命周期 Lua 变量必须被阻止或自动作废(呼应 §8.5 Detection/Frame 不能持久化 TaskLocal)。

#### 双层能力协商(§6.2、§3.5、ADR-005)

- **约束**：能力分 Backend Capability(Controller 能提供什么)与 Target Compatibility(某游戏/版本实测是否接受),Runtime 启动前同时校验两层;前者声明在 project.toml,后者写入 compatibility.toml;指纹变化自动降为 unverified(§6.2、ADR-005)。
- **对 Lua 迁移的启示**：命令式 Lua 无法静态推导脚本的能力需求,所需 capabilities 必须继续显式声明在 project.toml(不能从脚本推断)。Runtime 必须在创建/进入 Lua state 之前完成双层校验;校验不过则 Lua 根本不开始执行。

#### 严格后台守卫(§6.4、§3.8、ADR-011、§2.2、§27)

- **约束**：background_only 同时满足:不 SetForegroundWindow/SetFocus、不 SendInput/mouse_event/keybd_event、不 SetCursorPos、输入只投目标窗口、后台不可用即失败绝不自动切前台;有 Controller 审计日志(§6.4、ADR-011)。
- **对 Lua 迁移的启示**：强化动作白名单:Lua 可达面里不存在任何能抢焦点/全局注入的 API,失败=显式任务失败。即使脚本有 bug 或恶意也无法降级,因为该能力从未绑定。这是本框架相对 NKAS 等的核心差异点,必须在 grill 中作为不可退让项确认。

#### 取消/暂停/超时的注入(§9.2、§9.3、成功标准3/5、§19)

- **约束**：TaskStatus 含 Running/Pausing/Paused/Cancelling/…;pause 只在不可分割 Controller 调用结束后进入;取消检查点在轮询等待/截图等待/动作重试间/子任务边界/Vision 提交前后;普通等待 500ms 内响应取消;Controller 对已发 KeyDown/PointerDown 在暂停/取消/关闭时 best-effort 补 Up(§9.2、§19)。
- **对 Lua 迁移的启示**：协作式取消/暂停必须注入进运行中的 Lua:每个绑定动作调用是天然检查点(调用前后查 cancel token);另加 Lua debug/指令计数 hook 轮询取消,使不调用动作的纯 Lua 死循环也能在 500ms 内停下。暂停只能落在两次不可分割 Controller 操作之间→绑定调用需 yield/检查点。§9.2 依赖状态结构的分层超时需在绑定层重构为 per-capture/per-recognition/per-retry+全局预算。

#### 无 daemon / 单任务 / 每次运行独立(§9.4、§13)

- **约束**：Scheduler 简化为 CLI 一次只跑一个任务,无队列、无优先级、无后台常驻;定时用 Windows 计划任务调 CLI;Engine.start_task 每次一个 run(§9.4、§13)。
- **对 Lua 迁移的启示**：每个 task run 创建全新 Lua state(隔离+确定性+无跨运行泄漏),不保留常驻 Lua VM 当调度器。一个任务=一次 Lua 执行。时序由外部 Windows 计划任务驱动,不在 Lua 内实现 Cron/事件触发。

#### 变量类型/作用域与子任务无递归(§8.5)

- **约束**：变量类型 boolean/integer/string/duration;作用域 TaskInput/TaskLocal/StateLocal;Detection 和 Frame 不能持久化到 TaskLocal;CallTask 显式输入输出,v1 禁止递归(§8.5)。
- **对 Lua 迁移的启示**：TaskInput 来自 CLI,须在 Lua 边界校验/强制转换为这 4 种类型(Lua 动态类型无天然约束)。'v1 禁止递归'在 Lua 里被普通函数调用天然违反→需运行时调用深度守卫,或带论证地放宽(grill 议题)。Detection/Frame 不可持久化须靠句柄失效强制,而非 Lua 缺失的作用域类型系统。

#### 错误模型映射(§5.6)

- **约束**：AutomationErrorKind 枚举(Cancelled/Timeout/StaleObservation/ActionRejected/CaptureStalled/…);错误含是否可重试、来源模块、TaskRunId/StateId/FrameId、结构化上下文、面向用户短消息(§5.6)。
- **对 Lua 迁移的启示**：Lua 的 error()/pcall 必须在边界映射到 AutomationErrorKind;绑定动作抛出携带对应 kind 的类型化错误。'StateId' 字段在 Lua 无对应→用脚本 span/step id 做 trace 关联替代。可重试(retryable)与致命错误的区分对应 C++ 侧 Result/Status 模型(CLAUDE.md fail(...) 约定)。

#### Trace JSONL 事件类型与帧保存策略(§10.1-10.3、§3.2)

- **约束**：事件含 StateEntered/StateTransition/CaseMatched/FrameCaptured/ActionStarted... 带 seq+elapsed_ns,顺序不依赖墙钟;默认只存迁移帧/动作前帧/错误帧/低置信度帧,用 PNG 或 lossless WebP,不用有损;InputText 默认不写原文(§10.1-10.3、§15)。
- **对 Lua 迁移的启示**：Lua 模型须合成 StateEntered/StateTransition/CaseMatched 等价事件——具名 step/label span 或每次绑定调用自动 span。动作前帧由绑定层自动截取;elapsed_ns 取自单调时钟(绑死确定性、禁墙钟)。InputText 脱敏由绑定层强制,不交给脚本。

#### 项目资源包布局(§11、§26)

- **约束**：项目包=本地目录:project.toml+compatibility.toml+flows/+assets/templates/;不签名、不打包、不做 manifest.lock;模板按内容哈希加载;background_only 只接受与当前指纹/后端一致的 verified 记录(§11)。
- **对 Lua 迁移的启示**：Lua 脚本替代/并入 flows/(如 scripts/*.lua),由 project.toml 的 task 入口引用;模板按名解析到内容哈希资产;Trace 复制本次用过的模板到 resources/。project.toml 仍是能力/兼容性声明的唯一落点。

#### Engine API 语义不变(§13、ADR-008、§3.4、§3.7)

- **约束**：load_project/start_task/pause/resume/cancel/query_task/subscribe_events;领域操作语义须稳定;core/runtime 零 UI 依赖;subscribe_events 是 best-effort,lag 后须 query_task 取权威快照;引擎/Flow/Trace Schema 各自版本化(§13、§3.4、§3.7)。
- **对 Lua 迁移的启示**：Lua 是 runtime 内部实现细节,藏在 start_task 之后;Engine API 表面与 TaskStatus 生命周期必须不变,CLI(及未来 GUI)不感知 Lua。需为脚本引入 schema/版本标记(与 flow/v1 平行,§3.7)。core 层放 Lua 运行时不得引入任何 GUI 依赖。

#### 里程碑冲突与影响(§22、ADR-001)

- **约束**：M0(line707)是裸手写 demo,尚无任务模型,不受影响;M1(line716-724)是 Flow/状态机落地处,其退出条件 line723'所有状态和资源引用可在加载时验证'与命令式 Lua 直接冲突;M2 要求'核心无游戏专用分支'需在 Lua 下保持;ADR-001(显式状态机,理由:直观/可验证/易回放)正是被推翻的决策。
- **对 Lua 迁移的启示**：必须重谈 M1 退出契约为'已声明资源在加载期校验+可静态获知的引用被检查;完整可达性/终止性下沉为运行时预算'。ADR-001 需显式标记被取代并写新 ADR,论证 Lua 如何重新赢回'可验证/易回放'(回放靠 §ADR-006 的 Exploratory 帧复核仍在,可验证性是主要牺牲项)。

#### ADR-013 在本文档缺失(§24)

- **约束**：本 DESIGN.md v0.4 的 §24 只含 ADR-001…ADR-012(ADR-011 严格后台不降级、ADR-012 范围缩容),其后直接进入 §25,不存在 ADR-013。任务/grill 议程却引用 'ADR-001~013'。
- **对 Lua 迁移的启示**：grill 前必须澄清:要么 ADR-013 在更新版 DESIGN 中(本文件未含),要么 grill 议程预设了一条待写的新 ADR(极可能就是'采用命令式 Lua 任务模型'本身)。不能把 ADR-013 当作本文档已有的权威条款引用。

#### 核心零游戏分支(§2.4、§3.4、§22 M2、§27)

- **约束**：核心不得出现游戏名判断或专用分支,游戏特有逻辑全部放项目包;通用性=公共截图/输入/识别/Trace/配置能力可复用(§2.4、§22 M2、§27 分工表)。
- **对 Lua 迁移的启示**：此约束与 Lua 迁移天然契合:'不重编译写任务'正是把游戏逻辑从编译核心推入脚本,强化零分支。守卫点:C++ 绑定层不得内置任何游戏专用 helper/分支;所有游戏差异只能存在于项目侧 Lua 脚本与 project.toml/compatibility.toml。

---

## 2. 联网调研要点

> 四路联网调研的一手结论与对本项目的启示（来源链接随 summary 附带）。

### 2.1 竞品任务模型（SikuliX / Playwright / Selenium / AutoHotkey / 按键精灵）

#### SikuliX find/wait/exists 三态语义

- **要点**：find()=单次抛FindFailed;wait()=重试到超时(scan rate 3/s)抛异常;exists()=重试但返回None不抛;click()等动作内部每次重新find,不吃旧句柄。来源 https://sikulix-2014.readthedocs.io/en/latest/region.html
- **对本项目的启示**：Q1:动作内部re-find是'不对失效目标下手'的最简实现——UmbraFlow动作应在消费时刻重新验证目标,而非直接吃observe快照。三态分层(单次/重试/不抛)可作为Lua API find/wait/exists 的直接命名与语义蓝本。

#### SikuliX observe 事件模型

- **要点**：onAppear/onVanish/onChange 注册事件,observe()/observeInBackground(FOREVER)启动;每Region恰好一个observer可挂多事件;callback与queued两种消费模式可组合;onAppear若图已在则立即触发。来源 https://sikulix.github.io/docs/api/region/
- **对本项目的启示**：Q6:后台声明式弹窗监听的直接范本——注册'X出现→跑Y'由引擎并行观察,handler逻辑全在脚本侧,契合UmbraFlow严格后台+核心零游戏分支约束。observeInBackground佐证后台监听可行。

#### AutoHotkey ImageSearch/PixelSearch 命令式坐标

- **要点**：输出单个左上角坐标,未找到置空;ErrorLevel区分0命中/1没找到/2搜索失败(文件错误);无内建重试,等待靠Loop...Until+手写超时(A_TickCount或SetTimer)。来源 https://www.autohotkey.com/docs/v1/lib/ImageSearch.htm
- **对本项目的启示**：Q1/§8.3:一次一坐标输出与铁律一致。反面教材:观察与动作完全解耦、坐标是死值,新鲜度责任全甩给脚本作者=无租约。UmbraFlow必须避免。ErrorLevel区分'目标不在'vs'观察出错'值得借鉴——严格后台需区分失败类型不能混同。

#### Playwright Locator vs ElementHandle

- **要点**：Locator='怎么找到它'的描述,不持node引用,每次动作re-query天然带auto-wait/actionability(attached/visible/stable/enabled/receives events,重试到超时否则TimeoutError);ElementHandle=节点快照,DOM变即stale,官方劝退。force跳过非必要检查,trial只检查不动作。来源 https://playwright.dev/docs/actionability
- **对本项目的启示**：Q2最干净的二分:locator=无租约(永不stale)vs ElementHandle=隐式无限租约→stale。UmbraFlow租约必须显式:observe产出handle带世代号,动作消费时校验,过期严格失败不静默。actionability=租约续期条件。

#### Playwright addLocatorHandler 声明式中断

- **要点**：注册'overlay可见→跑handler',引擎在每次动作actionability re-check前检查;只在动作期间触发(passive不触发);handler后默认等overlay变hidden(noWaitAfter opt-out);times限次防死循环;handler耗时算入触发动作timeout;官方建议可预测弹窗inline处理,handler只留给不可预测的。来源 https://www.checklyhq.com/learn/playwright/handling-overlays-and-popups/
- **对本项目的启示**：Q6核心蓝本。照搬:(a)在动作re-check前检查而非passive;(b)handler后验证弹窗消失;(c)times上限;(d)handler耗时归属清晰利于可追踪;(e)可预测inline/不可预测handler分层。全部契合确定性+可追踪约束。

#### Playwright Dialog 默认 auto-dismiss(反面)

- **要点**：无listener时所有dialog自动dismiss;注册page.on('dialog')后必须自己accept/dismiss否则页面冻结。来源 https://playwright.dev/docs/dialogs
- **对本项目的启示**：Q6反面教材:默认auto-dismiss=静默降级,直接违反UmbraFlow'严格后台不静默降级'灵魂约束。UmbraFlow对弹窗必须显式处理并留trace,绝不默默吞掉。

#### Selenium StaleElementReferenceException

- **要点**：WebElement是指向具体DOM节点的reference,节点销毁即stale(即使新节点属性相同);只在动作那一刻才暴露;触发源=刷新/re-render/AJAX/modal;解法=永不缓存每次re-locate、WebDriverWait+expected_conditions、staleness_of+重取、retry ignoring stale。来源 https://www.browserstack.com/guide/stale-element-reference-exception-selenium
- **对本项目的启示**：Q2最直接反面教材:持有引用+延迟使用=租约过期,且失效只在使用点暴露。UmbraFlow若允许observe结果被后续动作复用,必须在消费点校验新鲜度并严格失败,绝不允许隐式长租约。

#### Robot Framework explicit/implicit wait

- **要点**：Wait Until Element Is Visible=explicit wait关键字,per-keyword timeout轮询到condition或超时fail;implicit wait全局;通用重试Wait Until Keyword Succeeds;该关键字DEBUG级每次记~45KB JS致日志膨胀。来源 https://www.neovasolutions.com/2022/08/12/how-to-handle-waits-implicit-and-explicit/
- **对本项目的启示**：Q1:关键字化DSL=声明式、可读、每动作独立求值,契合Lua命令式脚本目标。等待应per-action显式timeout而非全局implicit(implicit会让超时归属不清难追踪)。可追踪有成本——trace量需权衡。

#### 按键精灵/触动精灵 FindPic+循环头弹窗

- **要点**：FindPic返回序号(-1未找到)坐标写变参,相似度0.5~1偏色微调;后台找图Plugin.Bkgnd.FindPic不激活窗口返回'x|y';弹窗处理惯用法=Do...Loop循环体开头先FindPic/IfColor检测弹窗,命中Exit Do或点关闭;2014版附件找图存在'一次找到一次找不到'BUG。来源 https://zimaoxy.com/q/post/findpic/
- **对本项目的启示**：Q6:循环头手工轮询弹窗简单但易漏、非确定,正是UmbraFlow要用声明式handler取代的。§8.3:FindPic输出单坐标与铁律一致。后台找图印证严格后台可行。附件找图BUG=确定性/可复现反面教材,警示图像匹配需保证确定性。

### 2.2 sol2 沙箱与协程（yield/resume、指令钩子、沙箱逃逸、确定性消毒）

#### sol2 协程 yield/resume 边界 (Q5)

- **要点**：任务体做成 Lua 函数,宿主用 sol::thread::create(lua) 给独立栈,从其 state_view 取 sol::coroutine;每次调用=一次 resume 到下一个 yield。判活用 runnable()/operator bool/status():yielded=可续,ok=完成,error()=错。文档规范循环 for(...; n<N && co; ...)。来源: sol2 coroutine API https://sol2.readthedocs.io/en/latest/api/coroutine.html ; threading https://sol2.readthedocs.io/en/latest/threading.html
- **对本项目的启示**：Q5 草案'capture/wait/click 内部 yield、宿主在 resume 之间检查停止/暂停/预算'在 sol2 上可直接实现,resume 点即 §9.2 取消检查点,脚本作者零取消代码。

#### 协程生命周期/GC/dead 陷阱 (Q5)

- **要点**：跨帧存放 coroutine/thread 必须用 main_ 前缀引用类型(main_coroutine 等)绑定 main lua_State,否则置 nil 后被 GC 引用失效;社区实测 call_status::ok 后再调用不报 dead 反而重启 thread,故宿主循环须先查 status 再 resume;Lua thread 是 VM 协程非 OS 线程,无线程安全,跨 std::thread 访问同一 state 要自锁。来源: sol2 threading https://sol2.readthedocs.io/en/latest/threading.html ; issue #883 https://github.com/ThePhD/sol2/issues/883
- **对本项目的启示**：要'跨 resume 存活+全程 trace',main_coroutine 是硬约束;宿主调度循环必须显式 status 判定,不能靠协程自报死亡;停止标志跨线程置位需原子,resume 只在持有 state 的线程执行。

#### 硬中断:lua_sethook 指令预算防死循环 (Q5)

- **要点**：仅靠 yield 点挡不住不 yield 的 while true do end。标准做法 lua_sethook(L,hook,LUA_MASKCOUNT,N),超预算清钩子+luaL_error 展开;为防 pcall 吞 error,命中后改每1指令一钩再抛。关键坑:钩子不自动传入 coroutine(coroutine.resume(create(fn)) 逃逸),native C 调用不计指令。来源: PIL 23.2 https://www.lua.org/pil/23.2.html ; lua-l https://groups.google.com/g/lua-l/c/IPf2LK5FMWE
- **对本项目的启示**：Q5 的超时/失控必须加指令计数钩子做硬中断,且每次 resume 前给协程重装钩子;debug/coroutine 表须锁进沙箱(见 Q9),否则脚本可卸钩逃逸。这是 M0 必须配套的件。

#### sol2/sol3 沙箱建法与 _ENV 绑定 (Q9)

- **要点**：sol::environment(lua,sol::create) 建空 env,env['_G']=env 自引用;白名单全局逐个拷入 env[name]=lua[name];库表(string/math/table)须拷内容而非引用防污染宿主;env['bot']=handle 作唯一世界通道;lua.load(src,name,sol::load_mode::text) 强制文本挡 bytecode,再 env.set_on(func) 绑沙箱 env。来源: rubenwardy https://blog.rubenwardy.com/2020/07/26/sol3-script-sandbox/ ; lua-users SandBoxes http://lua-users.org/wiki/SandBoxes
- **对本项目的启示**：Q9 '只给 bot + string/math/table' 方向正确且可精确实现;库表必须拷贝(否则脚本改 string.* 污染宿主);set_on 与 load_mode::text 是两个不可省的绑定步骤。

#### 沙箱四大逃逸向量必堵 (Q9)

- **要点**：(1)默认 loader 把全局环境设到 chunk 直接逃逸——必须 set_on/load 第4参 env;(2)bytecode 无校验器可破内存安全,text模式+查首字节0x1B双保险;(3)debug 表 UNSAFE 能读写沙箱外变量,且能反装/卸载取消钩子;(4)setfenv/require/package 泄漏外部环境,Scribunto 因保留 package 表 loader 闭包偷渡 base 环境越狱。来源: lua-users SandBoxes http://lua-users.org/wiki/SandBoxes ; MediaWiki T49300 https://phabricator.wikimedia.org/T49300 ; codegenes https://www.codegenes.net/blog/how-can-i-create-a-secure-lua-sandbox/
- **对本项目的启示**：Q9 禁 io/os/package/require/load/loadfile 之外必须显式禁 debug(与 Q5 取消钩子冲突)和 setfenv;任何 loader 闭包不得携带真实 _G。白名单是唯一安全策略。

#### 受控子任务加载替代 require (Q7)

- **要点**：禁 require 后用 package.preload[name]=<已load chunk> 惯用法:require(name) 取到、不碰文件系统、惰性执行。映射 bot:load_subtask:宿主按名从项目包取源码→lua.load(text)→env.set_on 绑同一沙箱 env→缓存 chunk。须防 loader 闭包携真实 _G(Scribunto 根因)。来源: lua-l package.preload https://lua-users.org/lists/lua-l/2008-03/msg00357.html ; ericjmritz https://ericjmritz.wordpress.com/2013/03/13/sandboxing-chunks-of-lua-code-with-environments/
- **对本项目的启示**：Q7 子任务复用走宿主受控通道(bot:load_subtask),可记 trace 归属、能力门校验、独立测试,比裸 require 更符合可追踪+确定性;纯 Lua 内部拆函数(不跨文件加载)可并存,天然安全。

#### 错误模型 raise vs 返回值 + pcall 边界 (Q4)

- **要点**：社区惯例:可恢复返回 nil,err(对应 pcall false);硬失败 error() 抛。sol2 两层:protected_function/coroutine operator() 返回 protected_function_result 不抛,须 result.valid() 显式查(即 pcall 边界);只有 safe_script 替你抛。C++ 异常穿 Lua 要注册 sol::exception_handler_function,视构建定 SOL_EXCEPTIONS_SAFE_PROPAGATION,否则掉进 default_at_panic(VM 不可恢复)。来源: sol2 errors https://sol2.readthedocs.io/en/latest/errors.html ; exceptions https://sol2.readthedocs.io/en/latest/exceptions.html ; issue #841 https://github.com/ThePhD/sol2/issues/841
- **对本项目的启示**：Q4 分野与 sol2 机制天然对齐:租约失效/目标断开/guard 违规走 raise→宿主catch→写trace→判run失败;超时没匹配走返回 nil。每次 resume 后 result.valid() 是 pcall 边界。注意脚本 pcall 会吞取消 error,需每1指令钩子对抗。

#### 确定性隐藏通道消毒 (灵魂约束+Q9)

- **要点**：沙箱锁死后仍破坏确定性:pairs/next 顺序依赖表地址不确定(LuaJIT 维护者判定依赖即 bug);math.random 种子全局隐藏且算法平台相关(Redis 用每次 run reset 种子模型);float→string 走 libc sprintf 平台相关、超越函数 libc 相关、__gc/__mode/collectgarbage 引入 GC 非确定性。来源: LuaJIT #719 https://github.com/LuaJIT/LuaJIT/issues/719 ; Redis #95 https://github.com/redis/redis/issues/95
- **对本项目的启示**：'确定性+全量trace可复现'目标要求 M0 就:提供 sorted-key 确定性遍历 helper 或禁裸 pairs 依赖顺序;不暴露 math.random 改由 bot 提供受控随机(或 reset 种子);明确禁用/包装 float格式化、超越函数、GC introspection 等通道。

### 2.3 OCR 与分辨率自适应（引擎选型、确定性风险、自适应做法与坑）

#### OCR 引擎选型:HUD 数字场景

- **要点**：Tesseract(libtesseract)原生 C++、~10MB、清晰高对比文本 95–99%、CPU ~0.77s;裁 ROI + 字符白名单 tessedit_char_whitelist=0123456789 + 阈值/上采样后更快更准。PaddleOCR C++ 路径为 Paddle2ONNX 转 PP-OCRv4/v5 mobile → RapidOCR 骨架(DB后处理+CTC),链路重、转换须 dynamic shapes 否则结果偏移。固定字体 HUD 数字用小模板/小 CNN 往往又快又准。来源:IronOCR https://ironsoftware.com/csharp/ocr/blog/compare-to-other-components/paddle-ocr-vs-tesseract/、Paddle2ONNX https://paddlepaddle.github.io/PaddleOCR/main/en/version2.x/legacy/paddle2onnx.html、RapidOCR https://github.com/rapidai/rapidocr
- **对本项目的启示**：对应 §7.2『真实缺口出现再评估引入 ort』。能力应拆分而非单一 ocr 能力:vision.ocr.template_digit(确定性优先)与 vision.ocr.neural(后置)。M0 不引入。

#### OCR 确定性/可复现性(对四灵魂约束最要命)

- **要点**：根因浮点非结合性,ONNX 标准不描述算子求和顺序(arXiv 2501.05867 https://arxiv.org/pdf/2501.05867);非确定来源含多线程 reduction 顺序、ORT 无法固定 primitive 选择、FMA、x87 80-bit 中间精度(emmtrix https://www.emmtrix.com/wiki/Numerical_Precision_in_ONNX_and_AI_Inference)。Tesseract 算法确定但实操受线程/浮点/版本影响未必位级复现(harningle https://harningle.github.io/2025/07/26/tesseract-inference-fine-tuning-and-reproducibility.html)。可控化:ORT intra_op=1/inter_op=1/ORT_SEQUENTIAL+OMP_NUM_THREADS=1(默认 intra_op=0=物理核数,非确定),图优化 ORT_ENABLE_ALL 不引入 run-to-run 变异(https://onnxruntime.ai/docs/performance/tune-performance/threading.html);跨平台位级一致仅定点 Q16.16 可保,int8 量化 scale/zero-point 仍浮点(SpeyTech https://speytech.com/insights/fixed-point-neural-networks/);区分 run-to-run vs 跨硬件不变(Thinking Machines https://thinkingmachines.ai/blog/defeating-nondeterminism-in-llm-inference/)。
- **对本项目的启示**：神经 OCR 天然对抗『确定性+可追踪』。若引入,§6.2 compatibility 必须绑定模型哈希+ORT版本+线程配置,任一变化降 unverified;trace(JSONL)记录模型哈希/置信度/原始输出供 §6.5 离线复核。排序应低于分辨率自适应。

#### 分辨率自适应:业界做法

- **要点**：四类:基准分辨率归一化(单一 scale=live/reference,均匀缩放时最优、成本最低)、多尺度 scale-space 扫描(记 best scale 反投影,每加一尺度线性增耗时)、锚点+相对坐标(2+ 分离锚点解 scale+offset,单锚点脆弱)、coarse-to-fine 金字塔(低分辨率粗定位→全分辨率精修)。来源:PyImageSearch https://pyimagesearch.com/2015/01/26/multi-scale-template-matching-using-python-opencv/
- **对本项目的启示**：纯几何变换、确定性、与四约束零冲突。对应 §26 分工:框架负责坐标转换,项目声明 base 分辨率+ROI。适合作为 Q8 挂点的后置增量模块。

#### 分辨率自适应:坑

- **要点**：1) 下采样使子像素细节退化、相关峰塌陷到整数位置,特征<2px 时子像素插值失败;精定位须在原生分辨率+峰值拟合/双三次(templatematchingpy https://templatematchingpy.readthedocs.io/en/latest/guides/performance-tuning/、OpenCV https://answers.opencv.org/question/173114/sub-pixel-estimation-with-match-template/)。2) letterbox 黑边使屏幕百分比坐标失效,锚点须相对游戏视口、先减 offset 再除均匀 scale(SDL2 https://gamedev.net/tutorials/_/technical/apis-and-tools/stretching-your-game-to-fit-the-screen-without-letterboxing-sdl2-r3547/)。3) stretch-to-fill 需分别维护 X/Y scale,fill/crop 裁掉边缘元素。4) 逻辑 vs 物理坐标混用是 Windows DPI 经典 bug(MS https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-screenscaling)。
- **对本项目的启示**：§6.3 已列 DPI Awareness 为硬需求。compatibility.toml 应记录目标 scaling_mode(fit/fill/stretch/none)与实测 base 分辨率;trace 记录实际 scale/offset/视口矩形以保可追踪。

#### Q8 能力门与排序结论

- **要点**：Q8 草案(manifest 声明基准分辨率→宿主 run 前 fail-closed 校验)成立,分辨率自适应即插在此挂点,分两级:M0 立即做严格 live==reference 校验不匹配即拒(零成本、防缩放误匹配悄悄点错、契合不静默降级);后置模块加均匀缩放归一化(确定性纯几何,解 ok-script『严格拒太烦』痛点)。多尺度/子像素为更后档。
- **对本项目的启示**：Roadmap 排序:分辨率自适应(几何、零冲突、M0 立桩)优先于 OCR;OCR 引入时先 template_digit(确定性)后 neural OCR(独立后置、强制模型哈希+线程配置入 compatibility/trace)。

### 2.4 确定性与 trace（rr 决定论、record-replay、JSONL、fencing/租约）

#### rr 的确定性模型:只记录非确定性输入 → 界定 UmbraFlow「确定性」的可达上限

- **要点**：rr(https://rr-project.org/ ,ACM Queue https://queue.acm.org/detail.cfm?id=3688088)的立身之本:'most program execution is deterministic; only non-deterministic events need to be logged'。它单线程串行化以消除数据竞争,用硬件性能计数器精确定位注入点,回放时把记录的外部输入按原点重放,得到指令级 byte-exact 复现。关键:rr 的确定性是『给定相同输入→相同执行』,不复现外部世界本身。
- **对本项目的启示**：UmbraFlow 的『确定性』必然是这个弱化版本:能确定性回放的是『任务决策逻辑对给定 trace 输入的反应』,而非『重跑游戏得到相同结果』——游戏是不可控外部世界。这应写进 DESIGN 对确定性的定义,避免灵魂约束被误读为『重跑必同果』。推论:trace 必须把每次 capture/find 的输入侧(帧摘要、匹配结果、generation、时间)全量落盘,回放才能重演决策;Lua 脚本自身要保持纯函数式(禁 io/os/random,呼应 Q9),否则回放发散。

#### Playwright trace viewer:DOM 快照 rehydrate + 动作时间线 = trace 作为机器可读证据

- **要点**：Playwright(https://playwright.dev/docs/trace-viewer)把 pass/fail 变成取证录像:每个 action 前/中/后抓完整 DOM 快照 + 网络 + console + 源码行,打进单个 zip,viewer 把快照『复水』成可用 devtools 检查的活页面,按时间线可来回 scrub。新兴视角:trace 是『machine-readable evidence』,agent 可从中抽取失败 action+上下文提修复。
- **对本项目的启示**：佐证 Q2『确定性退化为运行期契约+全量 trace』这条退路是可行且业界主流的——只要 trace 够全。给 UmbraFlow trace 设计三条具体要求:(a) 每个动作记录 before/act/after 三态(§8.3 一次一坐标动作天然对齐『act 点』红点语义);(b) 记录『触发该动作的 Lua 源码行』以便回溯脚本决策;(c) trace 要设计成 GUI 调试窗(§9.4 只读消费者)和未来 agent 都能消费的结构化证据,不只是人读日志。

#### record-replay 的层次抉择:event-based 优于 time-based,semantic 优于 pixel

- **要点**：GUI R&R 文献(综述见 https://arxiv.org/html/2504.20237 ;event-based 专利 US10901810)两条铁律:(1) pixel/坐标捕获『prone to failure due to minor GUI changes』,semantic/widget 捕获更稳但需插桩;(2) time-based 回放会因随机性产生时间方差『change the desired behavior unexpectedly』,event-based 回放固定『顺序+输出』消除时钟变量,才能 intrinsically deterministic。
- **对本项目的启示**：对 Q2/§8.3 的正面支撑与警告并存:UmbraFlow 是纯视觉(pixel 层),先天落在最脆弱的一档——这正是为什么租约+每帧重校验不是可选项而是必需。回放 trace 时务必按『事件顺序』而非『墙钟时间戳』驱动,时间戳只作诊断元数据,否则回放会因非确定性时序发散。这也反向支持 Q1:显式 capture 一帧、对同帧多次 find,把『一次观察』锚成一个明确的 event 边界,比每识别器重抓更利于确定性回放。

#### JSONL 事件流惯例:固定信封 + data 内演进 + 立即 flush + 可选哈希链

- **要点**：NDJSON/JSONL 共识(https://jsonl.co/guide/json-vs-ndjson ,审计设计 https://arxiv.org/pdf/2601.20727):每行一个完整 JSON、
 分隔、UTF-8 无 BOM、尾换行;追加 O(1) 且崩溃只损坏最后一行。学界仪表日志收敛到固定顶层四字段(ts / run_id / event 标签 / data 载荷),schema 演进只在 data 内加字段;审计级加 prev_hash/curr_hash(SHA-256 over payload+前哈希,首条 GENESIS)做防篡改。事件标签保持开放但归为少数族(run 边界/item 边界/每次调用结果)。
- **对本项目的启示**：直接可落地为 UmbraFlow trace 规范:顶层信封固定为 { ts, run_id, event, data },event 标签分族(run 边界 / observe / act / interrupt / lease-fail / cancel);每事件写入即 flush(§取消时要能 flush 已发生事件,呼应 Q5);schema 版本号入信封。因『严格后台不静默降级』是灵魂约束,建议对 trace 上 prev_hash/curr_hash 哈希链——让『run 失败/租约失效/被取消』这类事件不可被事后抹除,契合可追踪+严格后台。escape 内嵌换行否则记录裂行。

#### EBR vs generation counter vs ABA:逻辑失效与内存/静默期是两个正交问题

- **要点**：并发文献一致(PLDI https://dl.acm.org/doi/abs/10.1145/3385412.3385978 ;https://grokipedia.com/page/ABA_problem):generation/version counter 解『逻辑 ABA』(值变了又变回来,指针相同但语义已换);EBR/epoch 解『内存安全/静默期』(在有人可能仍持引用时不回收)。核心警句:'SMR does not solve the logical ABA problem',解一个不解另一个,健壮系统两者都要。EBR 的软肋:回收依赖最慢线程推进,可致无界滞留。
- **对本项目的启示**：这是对 Q2『generation + 时间』最锋利的一刀:两维必须拆清各守什么。frame generation 应扮演『逻辑版本号』——单调、每 capture 必增,守的是『这个 detection 绑定的帧已被换掉』(直接类比 Selenium『新元素长得一样但不是同一个』)。而『时间』根本不解逻辑失效,它顶多解静默期(超时=久未 capture,租约作废重来),对应 EBR 的 quiescence 直觉。建议:generation 做主判据、fail-closed;时间做超时兜底。切勿让时间参与『帧是否换了』的判断,否则职责混淆。

#### Kleppmann fencing token:基于时间的租约保 liveness 不保 safety

- **要点**：Kleppmann『How to do distributed locking』(https://martin.kleppmann.com/2016/02/08/how-to-do-distributed-locking.html)论证:租约无法在任意进程暂停下保证安全——因异步模型下进程暂停/延迟无界,任何基于时间的安全保证都不可靠。唯一正确解:严格单调递增的 fencing token + 由资源端(存储服务)主动校验、拒绝 token 倒退的写。'the lock is only half the solution';时间只给『通常持锁一阵』的便利。
- **对本项目的启示**：对 Q2 的直接挑战 = 把校验放对层。Q2 说『click 在投递前重校验租约』——关键是**谁**在**哪层**校验。若宿主先校验 generation 再调注入,校验与注入是 back-to-back 两操作(见 TOCTOU 条),残留窗口仍在。Kleppmann 的教训:应让**最靠近副作用的注入层**带着 generation 一起提交、由它 fail-closed 拒绝陈旧 generation,才是 safety。时间戳则退位为纯超时便利。建议 API 形态:click(detection) 内部把 detection.generation 一路传到输入注入点做最后一道原子校验,而非在 Lua/宿主中间层校验后就当安全。

#### Selenium StaleElementReferenceException:observe→act 失效的最近现实同构

- **要点**：Selenium(https://www.browserstack.com/guide/stale-element-reference-exception-selenium)的 WebElement 是指向具体 DOM 节点的快照指针;框架 re-render/AJAX/导航/结构变更一旦替换或摘除该节点,指针即悬空,交互抛 StaleElementReferenceException。公认最佳实践:**存 locator(By)不存 element**,用前即时重新解析,配合显式等待稳定态;只对 stale 失败加小重试。CI/云端因时序不同更易触发。
- **对本项目的启示**：这是 Q2 的成熟工业先例,几乎逐条对应:UmbraFlow 的 Detection ≈ WebElement(绑定某帧的坐标),frame generation 变化 ≈ DOM 节点被替换。它给出两条可直接借鉴的设计:(1)『存 locator 不存 element』→ Lua 里鼓励存『识别器引用 bot.templates.home』(呼应 Q3 资产入 manifest)而非长期持有 Detection,Detection 应短命、用完即弃;(2) 失效是可恢复错误、走小重试(重新 capture+find),对应 Q4 应把『租约失效』归为脚本可处理路径的一种(raise 后由 host 判 run 失败,或提供受控重试),而非直接判死。

#### TOCTOU browser-agent(同构研究):实测 + 更强的 transactional precondition 方案

- **要点**：arxiv 2603.00476(https://arxiv.org/html/2603.00476)正是『observe 快照→推理→对活页面落动作』的 TOCTOU 研究。违规条件:状态变了且动作目标绑定变了 Bind(a,sc)≠Bind(a,su);根因是『lack of an atomic check-and-use primitive』。缓解=投递前用 MutationObserver(DOM 结构/属性/文本)+ ResizeObserver(几何/遮挡,DOM 不变故 Mutation 抓不到)做同步屏障,触发率 100%→0%,残留窗口~0.13s。作者明说重校验『reduces risk but does not provide strict atomicity』,因校验与动作仍是两个独立操作、观察者信号异步投递。提出的更强原语:(a) 作用域 freeze;(b) **transactional interaction API——提交动作+precondition(DOM 属性摘要/目标身份/包围盒),浏览器仅当 precondition 仍成立才执行,否则返回结构化失败促重观察**。
- **对本项目的启示**：对 Q2 的最强正反双证:正面——重校验路线有实测背书(触发率归零),证明 Q2 值得做;反面——(1) 残留窗口不可消除,check 与 act 永远两操作,UmbraFlow 里『投递 click→游戏响应』之间世界仍可变,故必须配『动作后再观察确认』的闭环,不能指望租约单点解决;(2) 作者提的 transactional API『动作+precondition,资源端原子校验』正是 Q2 应追求的终态,与 Kleppmann 的 fencing 校验层同构——把 generation 作为 precondition 一路带到注入点;(3) 关键盲区:ResizeObserver 存在,是因为『DOM 没变但遮挡/几何变了 Mutation 抓不到』——纯视觉的 UmbraFlow 没有 DOM 观察者,frame generation 也只在重抓时才动,故 TOCTOU Type II(结构没变、数据/像素变了但未重抓)UmbraFlow 的租约根本抓不到,只能靠 §8.3 铁律『每次 act 前强制 capture』兜底。

#### 虚拟/逻辑时钟:把墙钟从确定性回放里剔除

- **要点**：确定性回放对时钟的处理(ITU 虚拟时间 https://www.ituonline.com/tech-definitions/what-is-virtual-time/ ;DRC 专利 US10908935):墙钟读取(Date.now/gettimeofday/RDTSC)是经典非确定源。两法:record 模式记录每次读值回放注入;或用『虚拟/逻辑时钟』——把应用时序与墙钟解耦,按定义事件递增而非读宿主。推荐工程做法:所有时间源藏到单一可注入 clock 接口后,record 记读值、replay 走逻辑计数器,使回放发散有界。
- **对本项目的启示**：直接指导 Q2 里『时间租约』的落地形态与 trace 的时间字段:租约超时不应读墙钟做判断(墙钟在暂停/回放下不可靠,呼应 Kleppmann),而应绑定到一个宿主控制的、按 capture/tick 事件递增的『逻辑时钟/预算计数』——这也天然满足 Q5『暂停期间预算冻结』(逻辑时钟在暂停时不推进,墙钟会)。trace 里应同时记『逻辑序号(用于回放驱动)』与『墙钟 ts(仅诊断)』两个字段,回放按逻辑序号而非 ts。这样 Q2 的『frame generation + 时间』其实应是『generation + 逻辑预算』,两者都单调、都可回放,彻底把墙钟逐出确定性边界。

---

## 3. Q1–Q10 逐题裁决

> 每题结构：草案立场 / 正方论据 / 反方与风险 / **裁决建议** / 灵魂约束核对 / 题间依赖 / 需开发者拍板。忠实搬运 grill agent 产出。

### Q1. observe() 语义:一次调用一帧 vs capture 一帧多识别器查询

**草案立场**

采用显式 bot:capture() 取一帧,对同一 Frame 句柄多次 frame:find(recognizer),下一次 capture() 才推进帧;放弃 observe(recognizer) 每识别器重抓的模型。

**正方论据**

- 确定性铁律(§3.1)正面支撑:'同帧+同识别器版本+同参数=相同结果'要求'同帧'是可锚定的一等名词。capture-then-find 让'同帧'显式化,多识别器决策('是 Home 还是 Result 还是弹窗?')都对同一像素缓冲求值,§3.1 干净成立;反观 observe-per-call,每个识别器看到的是不同帧,'Home or Result'的多路判定夹在两次截图之间→引入截图间竞态,天然非确定。就确定性而言 Model B 严格优于 Model A。
- record-replay 层次抉择底料(arxiv 2504.20237)明确反向支撑本题:event-based 回放需要明确的事件边界,'显式 capture 一帧、对同帧多次 find,把一次观察锚成一个明确 event 边界,比每识别器重抓更利于确定性回放'。显式 capture 正是把'一次观察'定义成一个可回放的 event 锚点。
- 成本与不可变性:单帧 8 MiB(作者),§5.2 Frame 为 Arc 共享+缓存有上限、§3.6 Frame 创建后不可改——一次 capture、N 次 find 避免为单个决策付 N×8MiB 重抓;且帧不可变保证多次 find 之间像素不变,同帧多查是安全的。
- 可追踪(§3.2/§10.2)+JSONL 信封底料:capture-then-find 天然产出一条 FrameCaptured(带 frame_hash+generation)父事件 + N 条 Recognition 子事件全部引用同一 frame_id/generation,形成干净的父子 trace 树,一次观察=一个事件边界;rr 底料要求'每次 capture/find 输入侧全量落盘',Model B 的显式帧锚点让'这次决策到底用的哪帧'无歧义,Model A 则为单个逻辑决策散落 N 条 FrameCaptured,归属含糊。
- SikuliX 三态语义底料(find 单次/wait 重试/exists 不抛)提供了成熟的命名与分层蓝本,可直接叠在显式 capture 之上:frame:find=对已取帧的单次确定性求值,bot:wait=内部轮询重抓的循环,bot:exists=单帧一次不抛。无需自创语义。

**反方与风险**

- 作者自陈的核心代价:§8.3 铁律从结构强制退化为约定,脚本可把 frame 存进变量、跨一次 click 复用旧帧去 act。Playwright Locator vs ElementHandle 底料是直接反面教材——ElementHandle=持 node 快照、延迟使用即 stale,官方劝退,改用每次 re-query 的 Locator;Selenium StaleElementReference 同构(存 element 不存 locator=悬空)。持有 Frame 句柄正是业界公认的反模式。
- API 面变大:capture+find 两个名词比单个 observe 误用面更大。AutoHotkey ImageSearch 底料警示:观察与动作解耦、坐标成死值、新鲜度责任全甩给脚本作者=无租约;Robot Framework implicit wait 底料同理,归属不清难追踪。若租约不严丝合缝,Model B 会滑向这个反面。
- 确定性隐藏通道:多识别器平局若交给脚本自己写 if-else 顺序,顺序写错即非确定(底料:多规则命中用声明顺序、置信度相同退化为声明顺序)。裸 frame:find 链不强制平局裁决,需 binding 层补一个有序 helper。
- 替代方案 Model C(保留单名词 observe() + 内部帧缓存,§5.2 缓存复用最近帧):省一个名词也省重抓,但把帧边界对脚本和 trace 双重隐藏→损可追踪与确定性回放(record-replay 底料要求显式 event 边界),且 §8.3'动作后作废'更难推理。综合灵魂约束劣于 Model B,应显式否决而非默认。
- 跨 pause/cancel 持帧风险:逻辑时钟/预算在暂停时冻结(Q5),恢复后旧 frame 的 generation 与新世界错位;若不在 resume 作废(§9.2 恢复作废观察),回放会发散。持有句柄模型放大了这个窗口。

**裁决建议**

采纳草案 Model B(显式 capture + 同帧多 find),但用结构化补偿把 §8.3 从'约定'重新拉回'强制',关键是让 Frame 句柄对动作一次性失效,而不仅是 Detection 失效。

API 形态(sol2 绑定):
- bot:capture() -> Frame:返回只读 userdata;单调帧 generation 每次 capture 必 +1(EBR 底料:generation=逻辑版本号);自动 emit FrameCaptured{seq, elapsed_ns, generation, frame_hash}。
- Frame:find(recognizer) -> Detection|nil:对本帧的单次确定性求值,无重试(帧固定);nil=本帧未命中;自动 emit Recognition{frame_generation, recognizer_version, score, matched}。Detection 内挂租约{session_id, target_generation, frame_generation, expires_at}(交 Q2)。
- Frame:match({home,result,popup}) -> label, Detection:多识别器决策的有序 helper,严格按声明顺序求值、平局按声明顺序裁决(§3.1),把确定性平局关进 binding 层而非脚本。
- bot:click(detection)/swipe/… :在最靠近注入的层校验租约(Kleppmann fencing 底料:generation 一路带到注入点原子校验);成功后 bump generation→作废所有存活 Frame/Detection;emit ActionStarted(附动作前帧)/ActionCompleted。

宿主行为(结构化守 §8.3):
1. 任一输入动作后 generation 递增→所有在手 Frame/Detection 变陈旧;对旧 Frame 再 find() 或用旧 Detection click() 一律 raise StaleObservation,而非静默点击。'脚本拿旧 frame 去 act'从'能编译过'变成'必然抛错'。
2. max_action_frame_age(§8.3 默认 750ms,项目只能调短):Frame 超龄即使无动作介入也不能支撑坐标动作,覆盖'脚本在长纯-Lua 计算后拿旧帧下手'。
3. generation 是租约主判据、fail-closed;逻辑预算/时间仅作超时兜底(EBR 底料:两维各守其职,时间不参与'帧是否换了'的判断)。
4. 跨 pause/cancel 的 Frame 在 resume 作废(§9.2 恢复作废观察)。

便利封装(SikuliX 三态,内部自带 capture,是 Q5 yield 点所在):
- bot:wait(recognizer,{timeout,poll}) -> Detection:轮询循环,每轮重抓(generation 递增),迭代间 yield 供取消;超时 raise Timeout(接 Q4)。
- bot:exists(recognizer) -> Detection|nil:单帧一次、不抛。

规范循环(§26)由此结构强制:capture→find→click→(旧 frame 死)→必须重新 capture。

**灵魂约束核对**

确定性:Model B 让'同帧'成为显式一等名词,frame:find 是 (帧像素,识别器版本,参数) 的纯函数(§3.1);多识别器决策共享同一缓冲无截图间竞态;平局由 Frame:match 在 binding 层按声明顺序裁决,不交脚本;generation 每 capture 单调递增;持帧跨 pause 在 resume 作废——比 Model A 更确定。可追踪:每 capture 一条 FrameCaptured(frame_hash+generation)父事件、多条 Recognition 子事件引用同帧,一次观察=一个事件边界,动作事件附动作前帧,全部由绑定层自动 emit,脚本无法忘记埋点(§3.2/§10.2)。严格后台:本题只涉帧模型,与抢焦点/全局注入正交;click 仍走既有 background_only 注入路径,capture 不碰焦点,无新增可达面。核心零游戏分支:capture/find/click/match 均为通用原语,识别器/模板来自项目 manifest(Q3),绑定层不含任何游戏名或专用分支。

**题间依赖**

- Q2(租约)——最紧耦合:Q1 把 §8.3 的守卫责任转嫁给租约。'Frame 句柄动作后失效'本质是 Q2 机制;Q1 的结论只有在 Q2 把租约绑到 frame_generation 且在注入层 fail-closed 校验(Kleppmann fencing)时才成立。裁决互为前提。
- Q4(错误模型):frame:find 本帧未命中=返回 nil;旧帧再用=raise StaleObservation;bot:wait 超时=raise Timeout。Q1 的三态直接落到 Q4 的 nil-vs-raise 分野上。
- Q5(取消/协程):bot:wait 的内部轮询循环在迭代间 yield=Q5 的取消检查点;capture 的帧必须在 pause/resume 作废(恢复作废观察)。Q1 的便利封装就是 Q5 yield 点的宿主。
- Q6(interrupts):bot:on(popup,handler) 需定义 interrupt 扫描用主流程同一帧还是自带一次 capture。建议在观察周期边界做独立 capture、handler 拿自己的帧,这与 Q1 的 capture 节奏耦合。
- Q10/§10(trace schema):FrameCaptured/Recognition/Action 事件的信封与字段(generation、frame_hash、逻辑序号 vs 墙钟 ts)需与 Q1 的帧模型对齐。

**需开发者拍板**

第一条真实日常任务 / §26 M0 场景的真实决策结构:一次决策典型要查几个识别器?这直接验证'同帧多查'的效率前提是否成立,以及 capture→multi-find→click→recapture 是否匹配卡厄思梦境的实际流程(作者已在文档 132–135 行标注真机当前在角色详情页、非 §26 起点,此点必须开发者补)。其余需拍板项:(1)是否把 raw frame:find 暴露给脚本作者,还是只给 bot:wait/exists/match 便利封装、把裸 capture 藏进宿主(人体工学 vs 控制面的取舍);(2)max_action_frame_age 的实际值(§8.3 默认 750ms),需按目标游戏帧率/识别延迟由开发者定;(3)是否接受用两个名词(capture+find)换确定性/trace 红利,还是坚持单名词 observe()+内部缓存(Model C)——这关乎产品实际用法,只有开发者知道。

---

### Q2. 命令式 Lua 里怎么保住"不对失效观察下手"(租约 = frame generation + 时间)

**草案立场**

find() 返回的 Detection 带租约(绑定 frame 的 generation + 时间);bot:click(detection) 投递前重校验,若来新帧/generation 变/超时则 click 失败(返回 err/raise),脚本必须重新 capture+find;于是"确定性"从状态机的加载期证明(可达性/终止性)退化为运行期契约 + 全量 trace。

**正方论据**

- DESIGN 既定契约的忠实落地,非新发明:§8.3 一次一坐标铁律 + §5.4/ADR-004 已定义 ObservationLease{session_id,target_generation,frame_id,expires_at}、默认 max_action_frame_age=750ms(项目只能调短)、Controller 投递前二次校验 lease/generation/边界/句柄。草案只是把这套硬约束映射到 sol2 绑定层,与 DESIGN 完全同向。
- 工业界成熟同构 + 实测背书:Selenium StaleElementReferenceException 的公认解法是'存 locator 不存 element、用前即时重解析';Playwright Locator(无租约、每次 re-query 永不 stale)vs ElementHandle(节点快照即 stale)逐条对应——UmbraFlow 的 Detection ≈ ElementHandle,必须显式租约。TOCTOU browser-agent 研究(arxiv 2603.00476)实测:投递前重校验把违规触发率 100%→0%,直接证明'重校验路线'值得做且有效。
- generation 是解'逻辑 ABA'的正确工具(PLDI 并发文献):单调递增的 version/generation counter 专治'值变了又变回来、指针相同但语义已换',正对应'新帧长得像但不是同一帧'(Selenium'新元素长得一样但不是同一个')。frame generation 每 capture 必增 = 天然 fencing token。
- fail-closed 契合'严格后台 + 不静默降级'灵魂约束:失效即显式失败,对照 Playwright Dialog 默认 auto-dismiss(无 listener 就静默吞掉 = 静默降级)这个反面教材,租约 fail-closed 正是把这条约束落到每一个动作层,脚本即便有 bug/恶意也点不出去。
- '运行期契约 + 全量 trace'是可行且业界主流的确定性形态:Playwright trace viewer(每动作 before/act/after 快照 rehydrate = machine-readable evidence)、rr 确定性模型(只记非确定性输入→给定相同输入即相同执行)证明——只要 trace 够全,回放决策逻辑不必依赖加载期证明。这给草案'退化为运行期契约'这条退路以背书。

**反方与风险**

- Kleppmann fencing token 直击草案软肋:基于时间的租约保 liveness 不保 safety——进程暂停/延迟无界时任何时间保证都不可靠,唯一正确解是严格单调 fencing token 由资源端拒绝倒退。草案把'generation + 时间'并列且都算'租约'是危险的:时间绝不能参与'帧是否换了'的 safety 判断,否则职责混淆,残留窗口下会静默点错。
- generation 与时间正交,必须拆清各守什么(EBR vs generation counter,PLDI):'SMR does not solve the logical ABA problem'。generation 解逻辑失效(safety),时间/静默期解回收时机(liveness),两者解的是不同问题。草案用一个'租约'混两维 = 职责不清,埋下'时间没到但帧已换却放行'或'帧没换但超时误杀'两类 bug。
- 校验层放错则残留窗口不可消除(TOCTOU 研究):check 与 act 永远是两操作,重校验'reduces risk but does not provide strict atomicity'。若宿主/Lua 中间层先校验 generation 再调注入,是 back-to-back 两操作,残留窗口仍在。必须让最靠近副作用的 Controller 注入层带 generation 做最后一道原子校验(与 Kleppmann fencing 同构),而非中间层校验完就当安全。
- 纯视觉 TOCTOU Type II 盲区:UmbraFlow 无 DOM/Mutation/Resize 观察者,generation 只在重抓时才动。若结构没变、像素/数值变了但未重抓(血条掉了、倒计时数字变了、按钮 disable 了但位图相近),租约根本抓不到——租约非万能,必须配 §8.3'每次 act 前强制 capture' + '动作后再观察确认'闭环兜底。
- 加载期证明的净损失是真实代价(Q2 争议核心):命令式 Lua 控制流任意→可达性不可判定、终止性=停机问题,写错的脚本要到运行时才炸。租约补不回可达性/终止性,只能降级为 lint(best-effort)+ 运行时预算(全局 max_runtime + debug hook 指令计数)。必须诚实写进新 ADR、显式标记 ADR-001(显式状态机·可验证/易回放)被取代,不能假装没丢。
- 墙钟本身破坏确定性:读墙钟(Date.now/RDTSC)做超时判断是经典非确定源(虚拟/逻辑时钟文献);且暂停期间墙钟推进而预算应冻结(Q5)。'时间租约'若读墙钟,直接与'确定性'灵魂约束冲突,回放会发散。

**裁决建议**

采纳草案方向,做两处关键修正,落地形态如下。

【修正一:把租约拆成正交两维】
- generation(safety·主判据·fail-closed):Detection 携带 frame_generation,单调、每 capture/每输入动作必 +1。这是 fencing token,唯一决定'帧是否被换掉'。
- 逻辑预算 age(liveness·超时兜底):不用墙钟,用宿主控制的 logical_tick(按 capture/tick 事件递增,暂停期间冻结,呼应 Q5)。仅做'久未重抓则作废重来',绝不参与 safety 判断。

【修正二:校验下沉到 Controller 注入层(fencing)】
API 数据流:
  local frame = bot:capture()                  -- gen=G(++)、记录 tick=T
  local d = frame:find(bot.templates.home)      -- Detection userdata{gen=G, tick=T, rect, score};只读、短命
  bot:click(d)                                  -- 把 d.gen 一路透传到注入层,不在 Lua/宿主中间层做'最终'校验
Controller.deliver(action, lease) 内(最靠近副作用处做原子校验):
  - lease.gen != current_generation           -> fail StaleObservation(raise, fail-closed)
  - current_tick - lease.tick > max_action_frame_age(逻辑 tick 数,非墙钟) -> fail StaleObservation
  - 边界/窗口句柄二次校验(§8.3/§5.4 既有)
  - 通过则投递;投递即 generation++,先前所有 Detection 自动失效

【Detection 生命周期强制】任一输入动作后 generation 递增;把 Detection 存进长生命周期 Lua 变量后再用 -> raise StaleObservation。跨 step 只能提标量(text/number/label),禁存 rect/句柄(§5.3/§8.5)。绑定层保证脚本无法手造裸坐标绕过 lease(依赖 Q9 沙箱只暴露 bot)。

【动作后闭环兜 Type II】click 成功后绑定层强制作废当前所有 Detection,下一个动作前必须重新 capture(§26 规范循环:观察→动作→作废旧 Detection→重新截图)。

【自动 trace(绑定层强制,脚本无法忘埋点)】find -> {gen, tick, template_hash, score, rect};click -> before/act/after 三态 + lease 校验结果 + 触发该动作的 Lua 源码行;lease-fail 归独立事件族、写入即 flush、上 prev/curr 哈希链(不可事后抹除)。回放按 logical_tick/seq 驱动,墙钟 ts 仅作诊断字段。

【错误映射(与 Q4 协调)】StaleObservation ∈ AutomationErrorKind,走 raise;但按 Selenium 惯例归'可恢复',host 提供受控小重试(重新 capture+find)而非直接判 run 死。

【净损失补偿】可达性降为 lint;终止性靠运行时预算(max_runtime 逻辑时钟 + Q5 指令计数硬中断);写新 ADR 标记 ADR-001 被取代,论证'可验证性靠全量 trace + 回放重演决策',而非加载期证明。

**灵魂约束核对**

确定性:generation 是纯单调计数(确定);超时改用宿主逻辑预算 tick(禁 os.time/os.clock/墙钟),暂停冻结;trace 按 logical_tick/seq 驱动回放、墙钟仅诊断。诚实承认加载期终止性证明净损失,以运行时预算(max_runtime + 指令计数硬中断)补偿,不假装无损。 | 可追踪:每次 find/click 由绑定层自动 emit 结构化 trace(脚本无法忘埋点),before/act/after 三态 + lease 校验结果 + Lua 源码行;lease-fail 独立事件族、写入即 flush、上哈希链不可事后抹除,契合'失败/失效不可被抹除'。 | 严格后台:租约 fail-closed,失效=显式 StaleObservation,绝不静默点击(对照 Playwright auto-dismiss 反面);且注入层根本不存在抢焦点/全局注入 API(能力即缺席),脚本即便恶意也降级不了。 | 核心零游戏分支:租约机制是通用 generation + 逻辑预算,C++ 绑定层零游戏判断;模板/ROI/阈值全在项目包 manifest(Q3),游戏差异只存在于项目侧 Lua + project.toml/compatibility.toml。

**题间依赖**

- Q1(帧模型)是硬前置:generation 从 capture()->frame 来。若 Q1 采'显式 capture 一帧、同帧多 find',generation 绑在 frame 上、Q2 直接可用;若改'每识别器重抓',generation 语义与失效时机都变。Q2 必须等 Q1 先定。
- Q4(错误模型):租约失效走 raise(StaleObservation)还是可重试返回值需与 Q4 分野对齐——本裁决倾向 raise 但归'可恢复'并由 host 提供受控重试,需 Q4 确认边界。
- Q5(取消/协程):逻辑预算冻结(暂停不推进)依赖 Q5 的协程 yield/resume + 逻辑时钟;resume 点即 lease 校验点与预算 tick 推进点。
- Q9(沙箱):Detection 短命失效强制 + 脚本不能手造裸坐标绕过校验,依赖 Q9 禁 debug 表(防卸钩/改环境)、只暴露 bot 通道。
- §8.3/§5.4/ADR-004:需确认既有 ObservationLease 字段(session_id/target_generation/frame_id/expires_at)如何映射到 Lua 绑定层,尤其 expires_at 墙钟语义 -> 逻辑预算 tick 的重定义。
- ADR-013 归属未决:grill 引用的 ADR-013(本 DESIGN v0.4 缺失)极可能就是'采用命令式 Lua 任务模型'本身,Q2 租约裁决与'ADR-001 被取代'的论证应挂在这条新 ADR 下。

**需开发者拍板**

1) max_action_frame_age 的单位与默认:DESIGN 定 750ms 墙钟,改逻辑预算后应重定义为'N 个 tick',还是保留'单调时钟毫秒 + generation 双轨'?需拍板逻辑预算的单位与默认值。 2) 租约失效是否允许脚本受控重试(可恢复)还是一律判 run 失败:直接影响日常任务健壮性与 Q4 分野,需第一条真实日常任务的形态才能定(见 grill 文末'第一条真实日常任务')。 3) 是否强制'动作后再观察确认'闭环来兜 TOCTOU Type II:每个动作多一次 capture 的成本值不值,需按 M0 场景(§26 在卡厄思梦境对应哪个游戏内流程,真机现在停在角色详情页)拍。 4) ADR-013 是否即'采用命令式 Lua 任务模型',以及 Q2 租约裁决与 ADR-001 被取代的论证挂哪个 ADR 编号——需开发者确认 ADR 结构。

---

### Q3. 识别器/模板在哪声明:Lua 里,还是 manifest/项目包

**草案立场**

模板 PNG + ROI + 阈值放项目包 manifest(TOML/JSONC),Lua 按名字引用 bot.templates.home;资产与代码分离、可喂标注工具;反对方案是全在 Lua 内联 template("home.png", roi)。

**正方论据**

- [§8.6 / §22 M1 line723 —— 赢回资源那一半] 命令式 Lua 使'资源引用可静态枚举'成为底料明确标注的[丢失]项(Lua 可动态计算模板名)。把模板/ROI/阈值声明进 manifest,就把'本任务引用的识别资产'重新钉成有限、可枚举的闭合集合:加载期可校验 PNG 存在、算内容哈希、ROI 在帧边界内、threshold∈[0,1]。这直接补回 M1 退出条件 line723 里'所有资源引用可在加载时验证'的资源那一半(可达性/终止性那一半仍丢失,归 Q2/lint)。若内联在 Lua,这一半也一起丢。
- [§7.4 + §10.1 + metadata 模板哈希表 —— 可追踪硬需求] §7.4'图片按内容哈希加载'、§10.1'使用过的模板按内容哈希复制到 resources/'、metadata.json 记'实际使用的模板哈希表'。manifest 给每个模板一个稳定 name→content-hash 映射,trace 能静态对齐'CaseMatched 用的是 home@sha256-…',离线复核工具(§6.5)不必运行即可解析引用。内联构造时名字只是 Lua 局部变量,归因只能运行时抓取,可追踪性退化。
- [竞品:Playwright/Selenium'存 locator 不存 element'] 底料 Playwright 最佳实践与 Selenium StaleElement 教训一致:存'怎么找到它的描述'(定位引用)、每次动作 re-query,不存节点快照。bot.templates.home 正是这种'识别器引用';manifest 声明 + Lua 按名引用天然落在业界验证过的稳健档,内联构造对应被官方劝退的 ElementHandle 快照。
- [§3.1 确定性铁律 —— 参数即静态常量] ROI/threshold 放 manifest 是字面量常量,天然消除'参数从哪来'的一整类不确定性;呼应底料'约束模板引用只能来自字面量查找表'。放 Lua 则可能 roi=compute_roi(...),即使 Q9 沙箱挡住不确定性源,也多出一条需要论证的计算路径。同帧+同识别器版本+同参数→同结果的前提里,'同参数'因此变成静态可证。
- [标注工具闭环 —— 产品可行性] 作者论据成立:截图→框选→写 manifest 是纯数据写入;ROI/threshold 埋进 Lua 表达式则要求标注工具做 AST 改写,对不上。'不重编译就能写任务'的工程闭环依赖资产可被非代码工具增删。
- [§2.4 / §11 / §22 M2 核心零游戏分支] 模板是游戏专用资产(§11 assets/templates/)。声明进 manifest 让核心只需一条通用'按名解析识别器'逻辑、无需理解 Lua 语义即可静态校验资产集合,强化游戏差异只在项目侧。

**反方与风险**

- [manifest 不是银弹:动态名引用仍破坏枚举] 底料明确'Lua 可动态计算模板名,无法加载期完整校验存在性'。即便声明进 manifest,bot.templates[computed_key] 或 find(t[key]) 里 key 运行时算,加载期'引用完整性'就破了。manifest 只保证资产集合可枚举,不保证每次引用都命中已声明资产——后者需额外约束(禁动态索引或运行时 fail-closed),否则'加载期可验证'被高估。
- [纯静态 manifest 表达力不足] 参数化识别(检查第 N 个格子、阈值随场景放宽)在纯静态声明下会被逼成丑陋展开(slot_1..slot_9)。换 Lua 的初衷就是表达力;若 manifest 完全不给参数出口,会把动态需求挤回'内联构造'的老路。需区分'识别资产(PNG,天然静态)'与'识别调用参数(ROI 可能想动态)'。
- [双份声明的漂移成本] name 在 manifest 声明一次、Lua 引用一次,存在同步点:manifest 删模板 Lua 仍引用(加载期能抓)、manifest 声明但没用(只能 best-effort lint 报 dead asset)。相比'全在 Lua 一处'多一处维护。此成本换静态可校验,通常值,但对小任务是真实摩擦(呼应 ok-script'严格太烦'的痛点)。
- [ROI 与坐标空间/分辨率耦合] ROI 是 FrameSpace 矩形(§5.3),依赖分辨率;底料分辨率坑(letterbox/stretch/DPI)表明孤立的 manifest ROI 跨分辨率会失效。manifest ROI 必须绑定 base_resolution 并与 Q8 fail-closed 校验联动,不能当成自足声明——这是耦合约束,削弱'manifest 一处搞定'的简洁性。

**裁决建议**

采纳草案,精确化为三层边界。Layer 1(资产声明,强制进 manifest):project.toml 或旁置 recognizers.toml 用 [[recognizers]] 数组声明 { name, kind=template|color|composite, template=相对路径, roi={x,y,w,h}, threshold, grayscale, base_resolution }。加载期(§8.6 扩展)校验:文件存在→算 sha256 入 metadata 模板哈希表;roi 在 base_resolution 边界内;threshold∈[0,1];composite 子引用存在;收集所有 name 成闭合命名空间。Layer 2(Lua 只读句柄表):sol2 把 bot.templates 绑成只读表,键=Layer1 的 name,值=opaque RecognizerHandle userdata(不可改 roi/threshold);__newindex 一律 raise、__index 未声明名返回 nil 并可选 raise。用法 `local d = frame:find(bot.templates.home_marker)`;find 只接受 RecognizerHandle,传字符串/临时表→raise InvalidResource(Q4 硬失败)。关键:不把 template(path,roi) 构造函数暴露进沙箱,碰识别资产唯一通道是 bot.templates.<name>(Q9'能力即缺席')。默认禁止动态索引 bot.templates[expr](lint 期禁 []),需要动态时走 Layer 3。Layer 3(参数化识别,M0 不做):manifest 声明带参模板(roi_param/grid),Lua 传 4 类标量 `frame:find(bot.templates.slot, {index=n})`,ROI 由宿主 C++ 侧按声明公式确定性计算+边界检查,不在 Lua 算。数据流:每次 find 由绑定层自动 emit RecognitionStarted/Completed{recognizer_name, template_hash, roi, threshold, confidence, frame_id, generation},CaseMatched 等价事件用 recognizer_name 归因;实际用过的 PNG 按哈希复制到 trace resources/。

**灵魂约束核对**

确定性:ROI/threshold 为 manifest 字面量常量,模板按内容哈希加载(§7.4)→同哈希同像素同 SAD 结果(§3.1);禁 Lua 内联构造消除'参数从不确定源来'的通道;Layer3 的 roi 计算留在 C++ 侧,不进 Lua。可追踪:name→hash 静态映射 + 绑定层自动 emit recognizer_name/hash/roi/confidence(脚本无法忘记埋点,呼应可观测性优先),trace resources/ 存实际哈希供 §6.5/§10.1 离线复核,复核工具无需运行即可对齐引用。严格后台:识别是纯观察,manifest 声明识别器不引入任何前台/注入面,RecognizerHandle 是 opaque 不含 controller 能力;识别失败走 §5.6 RecognitionFailed/StaleObservation 不降级——守点落在 Q9 沙箱。核心零游戏分支:模板/ROI/阈值全在项目包 manifest+assets/(§11),核心只有通用'按名解析识别器'逻辑,bot.templates 是通用机制,无任何游戏专用识别器硬编码(§2.4、§22 M2)。

**题间依赖**

- Q8(能力/分辨率门)——强耦合:manifest 的 ROI 必须绑定 base_resolution,Q8 的 run 前 fail-closed 分辨率校验是 ROI 有效前提;分辨率自适应将来在此挂点(底料 Q8 结论)。
- Q9(沙箱)——强耦合:'不暴露 template() 构造函数、bot.templates 只读、__newindex raise、库表拷贝防污染'全部依赖 Q9 白名单沙箱机制才能实现。
- Q1(帧模型):frame:find(recognizer) 的签名依赖 Q1 定的 capture 一帧→对同帧多次 find 模型;声明位置不改 Q1 但要与 frame 句柄一致。
- Q2(租约):find 返回的 Detection 带 generation 租约、短命用完即弃,呼应'存 recognizer 引用不存 Detection'(Playwright 存 locator 不存 element)。
- Q6(interrupts):bot:on(popup_recognizer,...) 的 popup_recognizer 应复用同一 manifest 命名空间的具名识别器。
- Q4(错误模型):引用未声明名/传错句柄类型→raise InvalidResource,归 Q4 硬失败侧。
- M1 line723 契约需改写为:'已声明识别资产在加载期校验存在+哈希;静态名引用完整性 fail-closed,动态索引走运行时快速失败',并写入取代 ADR-001 的新 ADR。

**需开发者拍板**

1) manifest 载体:project.toml 内 [[recognizers]] / 独立 recognizers.toml / 每个 flow 旁置声明——影响多任务共享识别器的组织方式。2) 是否 M0 就要 Layer3 参数化识别,还是先纯静态——取决于 line135 待补的'第一条真实日常任务'是否需要'第 N 格'类动态 ROI。3) 动态索引 bot.templates[expr] 的容忍度:完全禁止(只允许静态点号,最强静态保证但限表达力)还是允许但运行时 fail-closed——需开发者定红线。4) manifest ROI 坐标用 FrameSpace 像素@base_resolution 还是 NormalizedSpace(0-1):后者对分辨率更鲁棒但有子像素精度坑(底料分辨率坑),二选一需拍板。

---

### Q4. 错误模型:raise(pcall)还是返回 nil/false

**草案立场**

可恢复、脚本该处理的(超时内没匹配)→返回 nil/false;硬失败(目标断开、guard 违规、租约失效)→raise Lua error,宿主 catch、写 trace、判该 run 失败;对应 Rust 的 Result vs panic 分野。

**正方论据**

- [与既有 Rust/C++ 模型天然对齐] §5.6 已把错误定义为 AutomationErrorKind 枚举 + retryable 布尔 + 来源模块 + TaskRunId/FrameId + 用户短消息;CLAUDE.md 规定 C++ 侧一律 Result<T>/Status + fail(...) 在子系统边界记一次日志。Lua error→宿主 catch→映射 AutomationErrorKind→fail() 只是把同一分野延伸到脚本边界,零新概念。
- [sol2 机制现成、成本低(Q4 底料)] protected_function / coroutine 的 operator() 返回 protected_function_result,必须 result.valid() 显式查——这就是 pcall 边界,宿主每次 resume 后查一次即可。可恢复走返回值、硬失败走 error() 也是社区惯例(recoverable→nil,err;hard→error())。不必自造错误传播机制。
- [SikuliX 三态证明'调用点选动词'可行] find()=单次抛、wait()=重试到超时抛、exists()=重试但返回 None 不抛。同一识别能力按'缺席是否可预期'暴露三个动词,让脚本在调用点决定这次'没匹配是正常分支(nil)'还是'必须在场的前置(raise)'——直接回答作者'混用要划清哪类走哪条':不由宿主全局拍板,由动词表达。
- [Selenium/TOCTOU 背书'租约失效=raise 且可重试'] StaleElementReferenceException 只在使用点暴露、公认解法是小重试(re-locate);TOCTOU 研究把'目标绑定变了'视为可恢复、重观察即可。这支撑把 StaleObservation 设成 raise(绝不静默点击,守 §8.3)但 retryable=true、脚本可 pcall 后 re-capture——比作者草案把租约失效一律扔进'判 run 失败'更精准。
- [反面教材证明'动作失败必须 raise、不能静默 false'] AutoHotkey 用 ErrorLevel 置空、Playwright 无 listener 时 dialog auto-dismiss——都是把失败静默吞掉,直接违反'严格后台不静默降级'。故 click/swipe 等动作永远不返回 false=没生效,失败一律抛类型化错误,这条正是灵魂约束在 Q4 的落点。

**反方与风险**

- [致命缺口:脚本 pcall 吞取消/预算 error(Q4+Q5 底料)] 若取消、全局超时、指令预算耗尽都走普通 Lua error(),脚本一句 pcall(body) 就能吞掉,任务在被取消后继续跑——同时击穿取消语义、严格后台、确定性预算。纯两层模型(作者草案)没有覆盖这一档。必须新增 Tier-C'不可捕获的宿主控制信号',命中后 debug hook 改每 1 指令重装并重抛(PIL 23.2 模式)越过 pcall;协作路径则由 Q5 在 resume 之间'停止 resume'根本不进脚本可捕获域。
- [C++ 异常穿 Lua 是真实 footgun(Q4 底料)] C++ 侧抛出的异常要穿回 Lua 必须注册 sol::exception_handler_function,并按构建定 SOL_EXCEPTIONS_SAFE_PROPAGATION,否则掉进 default_at_panic → VM 不可恢复整个 run 崩。raise 路线依赖这条构建纪律,不是'写个 error() 就完'。
- [错误消息会漏确定性(确定性消毒底料)] 若 error 值是字符串且内嵌 table 地址型 tostring、或 pairs() 顺序 dump 的上下文,trace 里的错误行就不可 byte-exact 复现。→ 错误必须是结构化 table(kind 为固定枚举串、字段有序、无地址),不能是自由字符串。
- [全返回值(纯 nil,err)这个替代方案要否掉] 好处是显式无异常;但(a)每行判错极啰嗦,(b)仍无法强制取消(脚本可以不检查返回值继续跑),反而更危险。故不采用'全返回值';raise 对'不可忽略的失败'是必需的。
- [全 raise(含缺席也抛,SikuliX find/wait 风格)也要否掉] 若'没匹配到'也抛,脚本被迫处处 pcall,且违反 CLAUDE.md'预期查找未命中=正常控制流,不用 Result'。缺席必须是 nil,不是错误。
- [混用边界若由宿主全局定,脚本会与宿主打架(作者自陈)] 替代:边界由调用点动词表达(find/exists 返回 vs wait/require 抛),宿主只固定'动作失败必抛、控制信号不可捕获'两条硬规则,其余交给脚本选动词。

**裁决建议**

采纳草案方向,但升级为三层错误模型 + 调用点动词选择 + 绑定层 trace-on-raise,并把最终校验放到最靠近副作用处。

Tier A — 预期缺席(返回值,不抛,不算 AutomationError):
  local d = frame:find(bot.templates.home)     -- Detection | nil(本帧不在)
  if frame:exists(bot.templates.popup) then …  -- boolean
  作用于 Q1 显式 capture 的帧;单次求值;'没匹配'是正常控制流,不产生 error kind。

Tier B — 异常但可恢复(抛结构化 table,脚本 MAY pcall):
  local d = bot:wait(bot.templates.home, 5000) -- 超时抛 {kind='Timeout',retryable=false}
  bot:click(d)                                 -- 租约失效抛 {kind='StaleObservation',retryable=true}
                                               -- 控制器拒绝抛 {kind='ActionRejected'}
  动作永不返回 false;失败一律抛。StaleObservation/ActionRejected/CaptureStalled 等 retryable 由 §5.6 字段驱动通用重试。

Tier C — 宿主控制信号(不可被脚本 pcall 捕获):
  Cancelled / 全局 Timeout(预算耗尽)/ 指令预算超限 / InternalInvariant。
  实现:协作路径由 Q5 在 resume 之间停止 resume,根本不进脚本域;硬中断由 debug hook 命中后改每 1 指令重装并重抛,越过任何 pcall。

抛出值形态(Lua 可见,供 pcall 分支):
  error({ kind='StaleObservation', retryable=true, module='controller',
          frame_id=…, action_id=…, message='…' })   -- table 非字符串;kind 固定枚举串;无地址

宿主边界(每次 resume 后):
  auto res = co();                 // sol::protected_function_result —— 这就是 pcall 边界
  if (!res.valid()) { auto e = mapToAutomationError(res); … }
  经注册的 sol::exception_handler_function 把 C++ fail(...) 与 Lua error table 双向映射到 AutomationErrorKind。
  未被脚本 catch 的 Tier-B → run Failed(带 kind);Tier-C → run Cancelled/Failed,不可能以'已捕获'到达脚本。

关键规则:trace 事件由绑定层在抛出的瞬间发(ActionFailed/RecognitionFailed/LeaseFail),先于脚本 pcall——脚本即便吞掉 StaleObservation,失败仍留在 trace。可追踪不依赖'未捕获'。

**灵魂约束核对**

确定性:错误是结构化 table,kind 为固定枚举串,message/ctx 禁地址型 tostring 与 pairs() 顺序 dump → 同帧+同识别器版本+同参数产生 byte-exact 相同错误行,可复现。取消/预算判定走单调时钟 + 指令计数 hook(非墙钟、非 os.clock),trace 记逻辑 seq 驱动回放。 | 可追踪:绑定层在'抛出瞬间'即 emit JSONL 事件(before/act/after 三态,§8.3),先于脚本可能的 pcall;未捕获再补 Failed 事件——契合 CLAUDE.md'propagated failure 在边界记一次'。脚本吞错也无法从 trace 抹除失败。 | 严格后台:动作失败永远是 raise 不是静默 false/no-op,杜绝 AutoHotkey/Playwright 式静默降级;guard 违规抛类型化错误 → run Failed,绝不回退前台;Cancelled 属 Tier-C 不可捕获,再烂/再恶意的 pcall 也停不住取消后继续执行。 | 核心零游戏分支:错误 kind 就是 core 里固定的 AutomationErrorKind 枚举,绑定层不含任何游戏专用错误处理或分支;游戏专用的'catch StaleObservation→重观察'恢复逻辑全在项目侧 Lua,不进框架。

**题间依赖**

- Q2(租约):StaleObservation 的 raise 语义就是租约 fail-closed 的落地——click(detection) 内部把 generation 一路带到注入点做最后原子校验、陈旧即抛。Q4 的'动作失败必抛不静默'是 Q2 得以成立的机制,两题必须一致敲定。
- Q5(取消/协程):Tier-C 不可捕获信号完全依赖 Q5 的 resume 边界模型 + debug hook 硬中断;'pcall 吞取消'这个坑由 Q4(定为不可捕获)与 Q5(每 1 指令重装钩子)合力解决,单独任一题都补不住。
- Q9(沙箱):必须确认 pcall/error 仍暴露给脚本(Tier-B 重试需要),但 debug.* 与 hook 相关表锁死——否则脚本能卸掉取消钩子,Tier-C 的不可捕获保证失效。Q4 的强度上限由 Q9 的沙箱面决定。
- Q1(帧模型):Tier-A 的 find→nil 作用于显式 capture 的帧;'缺席=nil vs 错误=raise'这条线依赖 Q1 的 capture/find 二分先定。
- Q6(interrupts):handler 内 raise 的作用域未定——是只失败该 handler 还是整个 run?需 Q6 明确 handler 错误归属与 trace 区分。
- CLAUDE.md C++ 侧:Lua 边界映射到 Result<T>/Status + fail(...),'边界记一次日志'对应本题的 trace-on-uncaught 单次 emit;并需定 SOL_EXCEPTIONS_SAFE_PROPAGATION 构建标志。

**需开发者拍板**

未捕获的 StaleObservation 是否由绑定层内建小重试 N 次(Selenium 式 re-capture+find)后再判 run Failed,还是一律立即失败、重试完全交给脚本——取决于"第一条真实日常任务里 stale 有多频繁"的工效/策略拍板,我无法替你定。是否向脚本暴露原生 pcall(便捷重试)还是只给受控 bot:try(fn) 包装(保证无法捕获 Tier-C)——安全 vs 工效权衡,需你拍。各 kind 的 retryable 默认值(如 ActionRejected 到底可不可重试)依赖真实目标(卡厄思梦境)实测行为,需你给数据或先给保守默认。SOL_EXCEPTIONS_SAFE_PROPAGATION 构建标志与异常配置由构建负责人拍(取决于工具链/是否全程 -fexceptions)。

---

### Q5. 取消/暂停/超时怎么跨协程工作(yield/resume + 宿主插桩)

**草案立场**

任务主体跑在 Lua 协程里,每个 capture/wait/click 内部 yield;宿主在 resume 之间检查停止标志(Ctrl-C)、暂停请求、预算/超时,取消时停 resume→补发未释放的 Up→flush trace;脚本作者完全不写取消代码,协程 yield 点天然就是 §9.2 要的取消检查点。作者自陈争议点:sol2 协程封装边界、以及"暂停期间预算冻结、恢复作废观察"能否干净映射到 yield/resume。

**正方论据**

- 【sol2 直接可实现,非纸上谈兵】底料 sol2 coroutine/threading API:sol::thread::create(lua) 给独立栈、从 state_view 取 sol::coroutine、每次调用=一次 resume 到下一个 yield,判活用 runnable()/status()。作者草案『capture/wait/click 内部 yield、宿主在 resume 之间查停止/暂停/预算』在 sol2 上是现成模式,resume 点即 §9.2 检查点,零取消代码成立。
- 【检查点覆盖面天然对齐 §9.2】§9.2 要求取消检查点落在:轮询等待/截图等待/动作重试间/子任务边界/Vision 提交前后。若每个 world-touching 绑定调用都 yield,这些点全部由绑定层占据,脚本『无法忘记』埋检查点——与 §3.2『脚本无法忘记埋点』同构,把正确性从作者自觉变成结构强制。
- 【pause 落点天然安全】§9.2『pause 只在不可分割 Controller 调用结束后进入』。协程在两次绑定 Controller 操作之间 yield,宿主此刻持有 resume 权、in-flight Controller op 已完成——这正是 §9.2 要求的安全暂停边界,不需要脚本配合。
- 【500ms 取消 + 逻辑预算冻结几乎白给】底料虚拟/逻辑时钟(ITU、DRC 专利)+ Kleppmann:墙钟必须逐出确定性边界。宿主驱动 resume 的模型给了唯一节流点:暂停时不推进『逻辑预算计数』即冻结(墙钟会继续走,逻辑时钟不会),直接满足 §9.2『暂停期间预算冻结』,墙钟只当 trace 诊断字段。
- 【单线程协作=确定性上限】底料 rr:串行化消除数据竞争是确定性立身之本;Lua thread 是 VM 协程非 OS 线程,只要 resume 只在持有 state 的线程执行就无线程安全问题(底料 sol2 threading)。协作式协程比『OS 线程+flag』在确定性上严格更优。
- 【trace 单点 emit,顺序无撕裂】resume 只在宿主线程发生,Cancelling/Paused/Resumed/BudgetExceeded 等事件全部在 resume 之间由宿主写入,seq+elapsed_ns 全序、无半状态;取消时补 Up、flush 都落在明确的 finalize 点(底料 JSONL 惯例:写入即 flush、崩溃只损坏最后一行)。

**反方与风险**

- 【yield 挡不住不 yield 的死循环——必须加硬中断】底料 PIL 23.2 + lua-l:while true do end 永不调用绑定动作→永不 yield→宿主永不夺回控制→取消无法在 500ms 内生效。草案只写『预算/超时』但未指定机制。必须补 lua_sethook(L,hook,LUA_MASKCOUNT,N):超预算清钩子+luaL_error 展开。关键坑:钩子不自动传入协程(coroutine.resume(create(fn)) 逃逸)、native C 调用不计指令,故每次 resume 前必须给协程重装钩子。这是 M0 配套件,不是可选项。
- 【pcall 会吞掉取消 error】底料 Q4/#841:脚本若 pcall 包住动作调用,基于 luaL_error 的取消展开会被 catch 掉。标准对抗:命中后改『每 1 指令一钩』再抛,使 pcall 来不及重入用户代码就死掉。草案的『透明取消』与 Q4 的 pcall 边界正面耦合,必须联合设计,不能各自为政。
- 【debug/coroutine 库必须锁进沙箱,否则钩子被卸/子协程逃逸】底料 Q9 四大逃逸向量:debug 表 UNSAFE,能反装/卸载取消钩子;coroutine.resume(coroutine.create(fn)) 生的子协程逃出父钩子预算。故 Q9 沙箱必须对脚本移除 debug 和 coroutine 库,而宿主在 C++ 侧用 sol::thread 驱动协程——脚本既拿不到 debug 也拿不到 coroutine。Q5 的安全性完全寄生于 Q9 锁死这两个库。
- 【main_coroutine GC 陷阱 + 必须 status-first】底料 sol2 threading + issue #883:要跨 resume 存活且全程 trace,协程/thread 必须用 main_ 前缀引用绑 main lua_State,否则置 nil 后被 GC 失效;且社区实测 call_status::ok 后再调用不报 dead 反而重启 thread,宿主循环必须『先查 status 再 resume』,不能靠协程自报死亡。草案里朴素的 for(...; co; ...) 需要这套显式纪律。
- 【『恢复作废观察』yield/resume 不免费给】§9.2『恢复作废观察』:pause→resume 后脚本跨暂停持有的 Detection/Frame 句柄必须失效。但协程 Lua 栈上的 userdata 局部变量跨 yield 原样存活——yield/resume 本身不作废它们。此语义只能靠 Q2 的 generation(resume 时 bump target_generation 使旧句柄 StaleObservation)强制,不能指望协程边界。这正是作者担心的『能否干净映射』——答案是不能,须外挂 Q2。
- 【两套预算正交,不能混为一谈】指令计数钩子只计纯 Lua VM 指令;一个在 C++ 里阻塞 700ms 的 capture() 不累积指令预算。故必须两套:(a)指令预算(debug hook)防纯 Lua 失控;(b)时间预算(宿主侧 per-capture/per-recognition/per-retry 超时 + 全局 max_runtime 单调时钟)。草案把『预算/超时』一锅烩,落地会漏掉纯 Lua 死循环或漏掉 native 阻塞其一。
- 【pause 非瞬时 + 替代方案更差(需明确取舍)】pause 受最长不可分割 Controller op 限(如 swipe 2s 内不 yield→pause 最多等 2s),这符合 §9.2 但须写清:pause 有界非即时,cancel 可中途放弃+补 Up 更快。替代方案均劣:宿主反复调脚本 tick 函数=把脚本逼回状态机(推翻命令式 Lua 初衷/ADR-001);显式 bot:checkpoint() =负担甩给作者(违反『零取消代码』);独立 OS 线程+flag=Lua state 非线程安全、引入数据竞争非确定(底料 rr 反对)。协程是正解,但要承认 pause 延迟上界。

**裁决建议**

采纳协程模型,但强制配齐硬中断+沙箱+双预算三件套,并采用『请求-yield』数据流把阻塞挪出协程。具体形态:

1) 数据流(核心决定):协程内只跑纯 Lua 决策 + 发『请求』;所有 world-touching 与所有等待都在宿主 resume 之间执行。绑定函数 bot:capture()/wait(ms)/click(det) 用 sol::yielding/lua_yieldk 挂起,把请求(capture now / wait 500ms / click D)yield 给宿主;宿主带 per-op 超时、cancel、pause 执行,再 resume 回结果。好处:协程内永不发生 native 阻塞,指令钩子成为唯一需要看管的协程内机制。

2) 宿主调度循环(伪码):
```
auto th = sol::thread::create(lua);              // 存为 main_ 引用防 GC
sol::coroutine co = th.state()[entryFn];
arm_instr_hook(th, N);                            // LUA_MASKCOUNT
while (co.runnable()) {                           // status-first (#883)
    if (cancelToken.load()) break;               // Ctrl-C / Engine.cancel
    while (paused.load()) park();                 // 逻辑预算冻结:不推进
    rearm_instr_hook(th);                         // 钩子不继承进协程
    auto req = co();                              // resume 到下一个 yield
    if (!req.valid()) { emit(mapErr(req)); runFailed(); break; }
    auto res = host_execute(req, per_op_timeout, cancelToken); // 阻塞在此
    advance_logical_budget(); check_global_runtime();
}
finalize(): best-effort 补 Up(仅投目标窗口,绝不 SetForeground);flush trace(Cancelled/Paused/Completed/Failed)。
```

3) 双预算:(a)指令预算 lua_sethook(th,hook,LUA_MASKCOUNT,N),每次 resume 重装;超限→清钩子→改每1指令一钩→luaL_error 展开(穿透 pcall)→映射 AutomationErrorKind::Timeout。(b)时间预算宿主侧 per-capture/per-recognition/per-retry 分层超时(§9.2 分层超时在绑定层重构)+ 全局 max_runtime 单调时钟。

4) 取消:std::atomic<bool> 由 Ctrl-C handler / Engine.cancel 置位;宿主在 resume 之间查,指令钩子内也查(纯 Lua 循环 N 指令内被打断,标定 N 使 <500ms);唯一跨线程状态就是这个 atomic,resume 只在持有 state 的线程做。取消是不可被 pcall 捕获的特殊 raise。

5) 暂停:Pausing→宿主完成 in-flight host_execute→不再 resume→置 Paused→冻结逻辑预算。Resume 时经 Q2 bump target_generation,使脚本跨暂停持有的 Detection/Frame 变 stale(『恢复作废观察』靠租约而非协程)。每次状态迁移 emit trace。

6) 沙箱(Q9 联合):脚本既无 debug 也无 coroutine 库,协程全部由 C++ sol::thread 驱动,堵死卸钩子/子协程逃逸。协程/thread 全程持 main_ 引用绑 main lua_State,resume 前必 status-first。

**灵魂约束核对**

确定性:单线程协作协程消除数据竞争(rr 模型);预算走逻辑计数器、暂停时冻结不推进,墙钟逐出决策路径只留 trace 诊断字段(虚拟时钟/Kleppmann);指令钩子按 VM 指令计数,同输入→同计数→同触发点,可复现;取消是外部非确定输入,按 rr 作为『被记录的外部事件』处理,trace 标定命中位置,回放能重演到该点为止的决策。可追踪:yield/resume/pause/cancel/预算超限全部由宿主在 resume 之间单点 emit 结构化 JSONL(seq+elapsed_ns 全序、无撕裂),补 Up 与 flush 在明确 finalize 点;脚本拿不到 debug/coroutine,无法抑制或伪造这些事件。严格后台:cancel/pause 的 finalize 只做 best-effort 补 Up 且仅投目标窗口,绝不 SetForegroundWindow;因抢焦点/全局注入 API 在 Q9 根本未绑定,即使协程被强杀也碰不到前台,失败=显式任务失败绝不静默降级。核心零游戏分支:调度循环/协程/预算/钩子机制 100% 游戏无关的宿主 C++ 代码,无任何游戏名或专用分支,绑定层不含游戏专用 helper,全部游戏逻辑在 Lua 侧。

**题间依赖**

- Q2(租约/generation):『恢复作废观察』和『取消后旧 Detection 失效』由 Q2 的 generation 强制,不是协程边界给的;Q5 的 pause/resume 必须 bump target_generation,硬耦合。
- Q9(沙箱):MUST 对脚本移除 debug 与 coroutine 库,否则取消钩子被卸、子协程逃逸预算;Q5 的取消/超时安全性完全寄生于 Q9 锁死这两库——最强依赖。
- Q4(raise vs 返回):取消/预算超限=不可被 pcall 捕获的 raise(luaL_error 每1指令一钩再抛);超时没匹配=返回 nil。这条『不可捕获的取消 error』需与 Q4 联合定义,否则 pcall 吞掉取消。
- Q6(interrupts):handler 也在同一协程内跑,cancel/pause/预算须同样覆盖 handler 期间;handler 耗时应计入触发动作的预算(Playwright addLocatorHandler 模型)。建议 handler 走同一协程以共享预算与钩子。
- Q1(帧模型):yield 粒度取决于 Q1 的 capture/find 拆分——capture() yield,同帧 find() 是否 yield 需按 Q1 帧生命周期定义检查点位置。
- Q7/§9.2(子任务边界):子任务若是纯 Lua 函数,检查点就是其中的绑定调用;指令钩子须覆盖整棵调用树(per-thread 天然覆盖),须确认。
- §26/M0:M0 场景循环(Home→点击→等 Result→点击 Reset→等 Home)是首个验证『等待中 pause/cancel 在 500ms 内响应』的落点。

**需开发者拍板**

需开发者拍板的:(1)指令预算 N(每钩指令数)与全局 max_runtime/最大 slice 数的标定——多少纯 Lua 指令算『太久』才中断,平衡 500ms 取消响应 vs 开销;此值依赖『第一条真实日常任务』(作者自己也标为未知)。(2)pause 是否 M0 必需还是后置里程碑——§9.2 列了 Pausing/Paused,但 M0(§26 裸 demo)可能只需 cancel+timeout;若 pause 延后,M0 就不必现在做 generation-bump/resume 作废整套机器。(3)取消响应 SLA 确认——成功标准『普通等待 500ms 内响应取消』是否为 M0 硬指标,以及是否同样适用于纯 Lua 死循环中断(这决定指令钩子 N 的量级)。(4)Ctrl-C(CLI)与 Engine.cancel(API)是否共用同一取消路径(很可能是,但需确认 CLI 信号→atomic 的接线)。(5)per-op 分层超时(per-capture/per-recognition/per-retry)在 M0 是否全需还是先做子集——取决于 M0 场景/第一条真实任务的形态。

---

### Q6. 随机弹窗 interrupts 怎么声明(bot:on + max_hits)

**草案立场**

用 bot:on(popup_recognizer, handler) 注册任务级 interrupt,宿主在每个观察周期前先跑已注册的 interrupt 识别器,命中就执行 handler 再回主流程,配 max_hits 封顶(草案说对应 ADR-013)。

**正方论据**

- 工业成熟范本直接对应:Playwright addLocatorHandler 就是『注册 overlay 可见→跑 handler』,且给了四条可照搬的成熟约束——(a)在动作 actionability re-check 前检查而非 passive 触发、(b)handler 后验证弹窗消失、(c)times 上限防死循环、(d)handler 耗时归属清晰。UmbraFlow 的 bot:on 与之几乎逐条同构,说明这条路线经过实战检验。
- 声明式取代『循环头手工轮询』的非确定反面教材:按键精灵/触动精灵惯用法是 Do...Loop 循环体开头先 FindPic 检测弹窗(底料明确标注『简单但易漏、非确定』)。bot:on 把弹窗处理从『脚本作者自觉埋点』变成宿主强制保证,并由绑定层自动 emit trace——契合 §3.2『脚本无法忘记埋点』。
- 天然落在 §9.2 已有的取消检查点上:草案的『每个观察周期前检查』正好叠加在 §9.2 列出的检查点(状态轮询等待/截图等待/动作重试之间);handler 作为一次不可分割的 Controller 操作序列,pause『只在不可分割调用结束后进入 Paused』的语义可直接套用,无需为 interrupt 另造暂停边界。
- 与 Q1 帧模型复用、成本与确定性双赢:interrupt 检查应复用本观察周期那一张 capture 帧(Q1 的 capture-once/find-many),只是宿主在把帧交给脚本前先按声明顺序跑一遍 interrupt 识别器——等价于额外几个 frame:find,复用 §8.4 ordered 策略 + §3.1 置信度相同退化为声明顺序的裁决规则。
- handler 动作后主流程被迫重截图,正好是 §8.3 铁律的免费红利:『任一输入动作都会使当前 Frame/Detection 失效』——handler 一旦 click,本周期帧作废,主流程必须重新 capture。SikuliX onAppear『图已在则立即触发』佐证『周期边界同步检查』模型可行,不需要后台常驻观察者。
- 严格后台天然守住:handler 只能经 bot 句柄碰世界(Q9 沙箱),可达面里根本没有抢焦点/全局注入路径,即使 handler 有 bug 也降级不了。对比 Playwright Dialog 默认 auto-dismiss(底料反面教材=静默降级),UmbraFlow 的弹窗必须显式处理并留 trace。

**反方与风险**

- 【最硬的一刀】草案引用的 ADR-013 在 DESIGN.md v0.4 不存在:ADR 列表止于 ADR-012(§24,第 809 行核实为 ADR-012 范围缩容),§10.2 事件类型里也无任何 interrupt 事件。因此本机制没有权威设计基座,不能把 ADR-013 当既有条款引用——它要么在未提供的更新版 DESIGN 里,要么就是这条待写的新 ADR 本身。裁决前必须先确认『新写 ADR-013:任务级 interrupts』。
- 重入/死循环风险=丢失的终止性(§8.6)复现:常驻不消失的弹窗、或 handler 自身又触发该弹窗,会让『drain 弹窗』永不收敛(停机问题)。必须(a)max_hits 预算封顶且超限=显式失败;(b)handler 执行期间挂起 interrupt 检查防重入(Playwright 执行 handler 时禁用同一 handler);(c)handler 内纯 Lua 死循环还需 Q5 的 lua_sethook 指令预算硬中断兜底。
- 确定性隐藏通道:若 interrupt 从无序结构注册(依赖 pairs 顺序,底料 LuaJIT #719 判定依赖即 bug),或每次都新抓一帧检查(每周期双帧、generation 语义混乱),firing 顺序就不可复现。必须钉死:有序注册容器 + 复用本周期帧 + 声明顺序 first-match。
- trace 归属会塌陷:§10.2 现有事件族里没有 interrupt 事件,不加专用事件族 + interrupt_id span,弹窗 handler 的 click 与主流程 click 无法区分,直接违反可追踪与回放归属(Playwright trace viewer / record-replay 的 semantic 层都要求动作可溯源)。必须扩 §10.2。
- 被动/后台观察者替代方案(SikuliX observeInBackground、独立线程盯帧)诱人但要驳回:它破坏单帧确定性,且撞 sol2 线程安全(底料:Lua thread 非 OS 线程、跨 std::thread 访问同一 state 要自锁)。代价是放弃『弹窗真·实时响应』——两个检查点之间冒出的弹窗要等到下个周期才被看到(§9.2 语境约 500ms 级)。这个延迟是否可接受需开发者确认。
- handler 表达力与递归禁令(§8.5 v1 禁止递归、Q7)冲突:允许 handler 再 observe/act/调子任务,会把 handler 变成迷你任务,重新打开作用域与递归深度问题。需要调用深度守卫,且明确 handler 能否调子任务。

**裁决建议**

采纳 bot:on 的方向,但修正为『同步、周期边界、drain-to-fixpoint、显式失败』的具体形态,并配套新写 ADR-013 + 扩 §10.2 事件族。

API(recognizer 走 Q3 的 manifest 引用,不内联模板):
  local h = bot:on(bot.templates.reconnect_popup, function(ctx)
      bot:click(ctx.detection)   -- handler 只经 bot 碰世界
  end, { max_hits = 3, on_exhausted = \"fail\" })  -- fail(默认)| stop
注册发生在任务起始附近,默认 run 作用域存活;可选 bot:off(h)。

宿主行为(每次 bot:capture() 周期):
1) 抓新帧 F(推进 generation);
2) 按【注册顺序】对 F 跑已注册 interrupt,first-match-wins(§8.4 ordered + §3.1 平局裁决);
3) 命中:emit InterruptMatched{interrupt_id, frame_id, hit_count};查预算:
   - 超 max_hits → emit InterruptBudgetExceeded,raise AutomationErrorKind(on_exhausted=fail),绝不静默跳过;on_exhausted=stop 则注销该 interrupt 并继续(仍写 trace);
   - 未超 → 开子 span InterruptHandlerStarted;**handler 执行期间挂起 interrupt 检查(禁重入)**;handler 的 bot 调用挂在该 interrupt_id span 下;handler 动作遵 §8.3/租约;handler 返回后 F 已失效 → 回到步骤 1 重抓,把弹窗 drain 到不动点;
4) 无命中 → 把干净帧 F 交给脚本,脚本自己的 frame:find(...) 在 F 上继续。

trace(§10.2 新增事件族):InterruptMatched / InterruptHandlerStarted / InterruptHandlerCompleted / InterruptHandlerFailed / InterruptBudgetExceeded,均带 interrupt_id + hit_count + frame_id + seq + elapsed_ns(单调时钟);主流程事件保留 step/label span,interrupt 事件靠 interrupt_id 分流。

取消/暂停:每次 interrupt 检查与 handler 的每个 bot 调用都是 Q5 的 yield 检查点;handler 内纯 Lua 由 lua_sethook 指令预算兜底;pause 落在 handler 两次不可分割 bot 操作之间。

里程碑:不属于 M0(M0 是无任务模型的手写 demo,§26);随 Lua runtime+trace 落在 M1+。

**灵魂约束核对**

确定性:interrupt 检查是 (帧 F, 识别器版本, 注册顺序, hit_count) 的纯函数——有序注册容器 + 复用本周期帧 + 声明顺序 first-match + 固定 max_hits 预算 + 不读墙钟;drain-to-fixpoint 是确定性收敛。禁止从 pairs 无序结构注册 interrupt(底料 LuaJIT #719)。
可追踪:新增 InterruptMatched/HandlerStarted/Completed/Failed/BudgetExceeded 事件族,带 interrupt_id span + hit_count + frame_id + seq/elapsed_ns;handler 动作与主流程动作在 trace 里天然分流,回放/GUI 可区分;预算超限是被记录的事件,绝不静默抹除。
严格后台:handler 只能经 bot 句柄(Q9 沙箱)动作,可达面里不存在抢焦点/全局注入路径,handler 有 bug 也降级不了;max_hits 耗尽=显式 raise 判 run 失败(对比 Playwright Dialog auto-dismiss 的静默 dismiss,坚决不学)。
核心零游戏分支:bot:on 是通用机制,『哪些弹窗 + 怎么处理』全在项目侧 Lua + manifest 模板;C++ 绑定层不得内置任何弹窗名判断或专用 handler。

**题间依赖**

- Q1(帧模型)强耦合:interrupt 检查复用本观察周期那张 capture 帧才能便宜且确定;若 Q1 退回『每识别器重抓』,interrupt 模型会变贵且帧语义歧义。
- Q2(租约)耦合:handler 动作携带绑定 interrupt 帧的租约;handler 一旦 act,generation 变→主流程 Detection 失效正是租约机制;drain-to-fixpoint 依赖 §8.3『任一输入使 Frame/Detection 失效』。
- Q4(错误模型)耦合:InterruptBudgetExceeded 与 handler 硬失败→raise 映射到 AutomationErrorKind;handler『没匹配到弹窗』属正常(不 raise)。需新增或复用一个 kind。
- Q5(协程/取消)耦合:interrupt 检查与 handler bot 调用都是 yield/检查点;handler 执行期间挂起 interrupt 重入 + 指令预算钩子防 handler 纯 Lua 死循环;pause 落在 handler 两次不可分割操作之间。
- Q7(子任务)+ §8.5 v1 禁止递归:handler 能否调子任务、调用深度守卫,与 interrupt 禁重入是同一『无递归』问题。
- Q9(沙箱)耦合:handler 跑在同一沙箱 env、bot-only;注册表必须是宿主控制的有序结构。
- Q10(单任务/无 daemon):interrupt 是一次 run 内的同步机制,不是后台常驻观察者,与 §9.4 无 daemon 一致。
- 跨 Q 的 ADR 依赖:与『采用命令式 Lua 任务模型』的主 ADR(很可能就是被误引的 ADR-013 位置之争)绑定——interrupts ADR 的编号与主 ADR 编号需一起厘清。

**需开发者拍板**

1) 是否新写 ADR-013『任务级 interrupts』并作废对现有 ADR-013 的引用——草案把它当既有条款引用,但 DESIGN.md v0.4 只到 ADR-012,这条必须开发者拍。2) max_hits 默认值与 on_exhausted 默认策略(fail 还是 stop)——取决于第一条真实日常任务里弹窗的实际频率与性质。3) interrupt 是仅 run 作用域,还是可按 step/阶段启停(有些弹窗只在特定阶段预期)——取决于首条真实任务的流程结构。4) handler 能否调子任务、允许多深——耦合 Q7,需开发者定边界。5) 命中弹窗能否『中止主流程判 run 失败』(致命弹窗)还是只能 handle-and-continue。6) 第一条真实日常任务里具体有哪些弹窗(重连?每日奖励?更新提示?)——文档 §26/待补节自己都标注 M0 场景与首条任务未知,这是验证 API 表达力的前提,只能开发者提供,不替他假设。

---

### Q7. 子任务/复用:Lua 函数互调,还是宿主级任务注册表

**草案立场**

任务=一个 Lua 函数;子流程靠 require 或直接调用别的 Lua 函数复用;不做宿主级 CallTask/Return(视其为状态机时代产物)。

**正方论据**

- 【调用语义退化有底料背书】DESIGN 动作白名单明确把 SetVariable/CallTask/Return/StopTask『退化为 Lua 原生(local/函数调用/return/error)』,不再是绑定动作。所以『call/return = 原生 Lua 函数调用』这半边草案是对的:零绑定面、最简单,契合 CLAUDE.md『简洁优先/最小影响』——不该为子流程再造一个宿主级 CallTask 动作。
- 【强化核心零游戏分支(§2.4/§22 M2/§27)】子流程逻辑全部落在项目侧 Lua,C++ 绑定层不内置任何 CallTask/子任务 helper,正是『不重编译写任务』把游戏逻辑推入脚本的目标,反而比状态机时代更彻底地守住零分支。
- 【工业先例支持函数级组合】Playwright 等成熟框架的复用就是普通函数/Locator 组合,没有框架级『子页面』机制;§8.5 本就要求 CallTask『显式输入输出、v1 禁递归』,即把子流程约束成一次有限调用——普通 Lua 函数天然贴合这个『有限、显式』意图。
- 【作用域用原生 Lua 表达最自然】TaskLocal/StateLocal 用 Lua local/闭包即可表达,不必再造作用域类型系统;§8.5 的 4 种标量类型(boolean/integer/string/duration)只需在『跨子任务边界』处强制,而非在语言层重建。

**反方与风险**

- 【致命内部矛盾:require 违反沙箱铁律】草案『用 require 复用』与 Q9 冲突。底料[沙箱四大逃逸向量]:require/package/loadfile 是四大逃逸向量之一,Scribunto(MediaWiki T49300)正因保留 package 表、loader 闭包偷渡真实 base 环境而越狱。require 分支必须整条删除,复用只能走宿主受控通道。
- 【raw require 破坏确定性】require 在任意时刻碰文件系统、加载顺序不定、package.loaded 全局隐藏缓存——都是非确定源(呼应 rr『只记录非确定输入』、Lua 须纯函数式)。只有宿主按名→内容哈希资产(§11)解析、一次编译缓存,才能满足『同帧同版本同参数→同结果』与全量 trace 可复现。
- 【可追踪净损失】纯 in-file 函数调用在 JSONL trace 里与主流程无边界,无法合成状态机时代 StateEntered/CallTask/Return 的等价 span(§10.1、底料[Trace 事件类型])。草案自列的『子流程 trace 归属』正是纯函数做不到的——子流程内的 observe/act 事件没有父 span 归属,GUI 调试窗(§9.4 只读消费者)无法折叠/回溯。
- 【§8.5 v1 禁递归被天然违反且草案无守卫】Lua 函数调用允许无限递归/相互递归,加载期是停机问题不可判定。底料明确『v1 禁止递归在 Lua 里被普通函数调用天然违反→需运行时调用深度守卫或带论证放宽』,草案对此只字未提。
- 【独立测试是纯函数的弱项】草案自列『能不能被独立测试』——纯 local 函数不可独立寻址,无法用 mock bot 单测;而具名、宿主可加载的子任务天然可隔离测试。底料[受控子任务加载替代 require]:bot:load_subtask 通道『可记 trace 归属、能力门校验、独立测试,比裸 require 更符合可追踪+确定性』。
- 【Detection/Frame 跨边界泄漏,绕过租约】普通函数调用可把父流程的 Detection 句柄传进子流程延迟消费,绕过 Q2 租约与 §8.5『Detection/Frame 不能持久化 TaskLocal』,在消费点静默点击失效目标(Selenium StaleElementReference 同构)。边界必须强制标量-only + 句柄失效。

**裁决建议**

修正草案:把『调用语义』与『跨文件加载』拆成两层,保留原生调用、删掉 require、复用改宿主受控通道。

第 1 层 · 同 VM 内组合(允许,无宿主机制):任务体内用普通 Lua 函数/闭包做子流程,call/return 就是原生调用与 return——兑现动作白名单的『CallTask/Return 退化为 Lua 原生』。唯一附加:宿主装一个运行时调用深度守卫(阈值可配),命中即 raise TaskError{kind=RecursionLimit},兑现 §8.5 v1 禁递归。

第 2 层 · 跨文件/具名子任务复用(不用 require,走宿主通道):
  API(二选一或并存):
    local sub = bot:load_subtask(\"collect_rewards\")   -- 返回可调用句柄
    local ok, out = sub({ count = 3 })                 -- 显式输入表 → 显式输出(标量 only)
    -- 或一步式:local ok,out = bot:call_subtask(\"collect_rewards\", { count = 3 })
  宿主行为(package.preload 惯用法):
    1) 寻址:名字→项目包 scripts/<name>.lua 的内容哈希资产(§11);禁文件系统直读、禁路径参数;只接受 manifest 已声明的子任务名,未声明即 fail-closed 拒(借 Q8 挂点、呼应 §3.3 加载期校验已声明引用)。
    2) 加载:lua.load(src, \"@subtask:\"..name, sol::load_mode::text) 强制文本挡 bytecode(Q9 逃逸向量2);env.set_on(chunk) 绑『同一个』沙箱 environment(Q9)——子任务与父任务共享同一受限 bot 世界通道,拿不到真实 _G(防 Scribunto 越狱)。
    3) 缓存:chunk 一次编译、按本次 run 缓存(package.loaded 语义由宿主表实现,package 表本身不暴露)。
    4) 依赖图:宿主维护加载图,检测环→v1 fail-closed 拒绝(在加载边界兑现禁递归);子任务依赖可在加载期枚举(静态可判定→保留在加载期,呼应 §8.6『可静态判定的引用留加载期』)。
    5) Trace 归属:load/call 自动 emit span 事件 {event:\"subtask_enter\"/\"subtask_exit\", data:{name, parent_span, seq, elapsed_ns}},合成 CallTask/Return 等价结构;子流程内 observe/act 事件带 parent subtask span id。
    6) 边界契约:跨边界只允许 §8.5 的 4 种标量(boolean/integer/string/duration),Detection/Frame 句柄禁止跨子任务传递,检测到即 raise(TypeError/StaleObservation),兑现 Q2 租约 + §8.5。
    7) 能力门:子任务需要的额外能力/资源并入 project.toml 能力集合,run 前双层校验(§6.2);不从子任务源码推断能力。

本裁决不是『复活状态机 CallTask 动作』:call/return 仍是原生 Lua,只有『跨文件加载』因 require 被禁而改为宿主受控——精确保留草案精神、只补掉它与 Q9 的矛盾。

**灵魂约束核对**

确定性:名字→内容哈希(§11)确定寻址,禁文件系统任意读;chunk 一次编译缓存;依赖图无环;子任务导出表禁 pairs 顺序依赖(用 ipairs/有序容器);无 package.loaded 全局隐藏状态外泄——run-to-run 可复现。可追踪:load/call 自动 emit subtask_enter/exit span,子流程 observe/act 带 parent span id,合成状态机 CallTask/Return 的等价 trace,脚本无法『忘记』埋点;每 run 独立无跨 run 泄漏。严格后台(不静默降级):子任务 env.set_on 绑同一受限沙箱,能力即缺席——子任务拿不到 io/os/前台注入/require,不可能比父任务拥有更多世界通道,越权即因『函数不存在』而失败;未声明子任务名 fail-closed 拒,不静默回退。核心零游戏分支:加载/调用是通用宿主能力,子任务源码全在项目包 scripts/,C++ 绑定层无任何游戏专用 helper 或分支。

**题间依赖**

- Q9 沙箱(强依赖,地基):load_subtask 必须 env.set_on 绑同一沙箱 env + load_mode::text + 不暴露 package/require——Q7 的安全性完全建立在 Q9 之上,草案已自认此耦合。
- Q4 错误模型:子任务边界的分野——租约失效/guard 违规/句柄跨界=raise;子任务内超时没匹配=返回 nil;每次 call_subtask 是一个 pcall/protected_function_result.valid() 边界。
- Q2 租约 + Q1 帧模型:Detection/Frame 禁跨子任务边界依赖 Q2 的 generation 失效机制与 Q1 的 capture 语义。
- Q5 协程/取消:子任务调用点是 §9.2 取消检查点;子任务内 capture/wait/click 仍 yield;lua_sethook 指令计数钩子须跨子任务 chunk 生效(每次 resume 重装钩子),否则子任务里的 while true 逃逸。
- Q8 能力门:子任务声明的能力/资源并入 manifest,run 前双层校验;未声明子任务名即拒——Q7 借用 Q8 的 fail-closed 挂点。
- §8.5 v1 禁递归:in-VM 函数递归是否加深度守卫需与该条一并敲定(见 developerInputNeeded)。

**需开发者拍板**

1) 递归策略拍板:§8.5『v1 禁止递归』对『同 VM 内普通 Lua 函数递归』是否强制——加运行时调用深度守卫(阈值取多少?)还是带论证放宽?这是纯 Lua 函数天然违反、必须开发者定的点。2) 跨文件复用是否 M0/M1 就需要:若第一条真实日常任务单文件足够,bot:load_subtask 可按 YAGNI 推迟到真实多任务共享子流程时再落地——需要开发者告知第一条任务的复杂度与是否跨任务共享子流程(与议程末尾『第一条真实日常任务』同一未知)。3) 子任务边界输出形态:单返回值 vs 多标量输出表,取决于真实任务数据流。4) 子任务命名/寻址是否需要项目内命名空间或版本标记(与 §3.7 schema 版本化平行)。

---

### Q8. 能力/兼容性门(分辨率/目标)在哪把关 + 要不要现在留自适应口子

**草案立场**

manifest 声明要求(基准分辨率/模板/能力),宿主 run 前对目标与分辨率做 fail-closed 校验、不匹配即启动失败(对应 §6.2 双层协商 + compatibility.toml);此处是分辨率自适应将来的挂点,但纠结现在就留自适应口子还是先严格后加模块。

**正方论据**

- 【门的位置=双层,与 §6.2 逐条对齐】DESIGN §6.2(line 299-304)已把能力拆成 Backend Capability(project.toml 声明)+ Target Compatibility(compatibility.toml 实测),且明文'Runtime 启动前同时检查两层'。Q8 的宿主 fail-closed 门就是这两层的落地执行点,不是新机制,是把既有契约接到 Lua 边界前:两层任一不过 → 根本不创建 Lua state。这直接补回了命令式 Lua 丢掉的加载期证明的一部分(能力/兼容/分辨率是可静态判定项,留在门里;可达性/终止性才下沉运行时,见 Q2)。
- 【门必须在 Lua state 创建之前】§8.6/line437 的加载期契约含'Backend Capability 已声明''Target Compatibility 结合当前目标版本检查'。底料[双层能力协商]明确:命令式 Lua 无法从脚本静态推导能力需求,所以 capabilities 必须继续显式声明在 project.toml、Runtime 在进入 Lua state 之前完成双层校验、不过则 Lua 根本不开始执行。这把 Q8 从'某处校验'钉死为'副作用发生前的唯一闸门'。
- 【指纹绑定 + 自动降级 unverified 已有现成机制】compatibility.toml(line 548-567)带 executable_sha256/file_version/window_class,§6.2 明文'版本/哈希/窗口类/后端变化后自动降为 unverified',§6.3(line 312)'窗口句柄/客户区尺寸/进程实例变化时递增 TargetGeneration'。分辨率(客户区尺寸)天然属于这套指纹——门只要把 live client rect 纳入指纹比对即可,复用既有失效链,不需新建判定逻辑。
- 【留挂点=零成本且 §27 已强制这道缝】§27 分工表(line 863)把'坐标转换'划给框架、'基准分辨率和 ROI'划给项目。这意味着框架侧本就必须存在一个 template-space→screen-space 的坐标转换层。把所有坐标动作都路由过一个 CoordinateTransform 对象(M0 恒等:scale=1/offset=0)几乎零成本,却让将来的自适应变成'只改 transform 计算'而非改租约+每个绑定+trace schema。底料[分辨率自适应业界做法]佐证:基准分辨率归一化是'纯几何变换、确定性、与四约束零冲突',后置增量安全。
- 【M0 就 fail-closed 严格拒有正面技术依据】底料[Q8 能力门与排序结论]:M0 立即做 live==reference 严格校验、不匹配即拒是零成本,且能防'缩放误匹配悄悄点错'——对应反面教材按键精灵 FindPic'一次找到一次找不到'BUG、相似度/偏色 misfire。严格拒 = 严格后台不静默降级的直接体现:宁可显式启动失败,不 stretch 一下赌'差不多能点中'。
- 【自适应本身有坑,不该塞进 M0 关键路径】底料[分辨率自适应:坑]:下采样使子像素细节退化、相关峰塌陷到整数位置(坑1);letterbox 黑边使百分比坐标失效需相对视口先减 offset(坑2);stretch-to-fill 需分别维护 X/Y scale(坑3);逻辑 vs 物理坐标混用是 Windows DPI 经典 bug(坑4)。这些证明自适应是真有难度的模块,M0 抢做只会用'差不多'掩盖真实 mismatch。

**反方与风险**

- 【ok-script 教训:纯严格拒在日常里很烦——这正是本题存在的理由】草案自己承认。每次游戏更新导致窗口尺寸漂移、换台带 DPI 缩放的笔记本、多显示器,都会让任务直接 brick。若门只有'拒绝'一档、又没预留自适应缝,使用者被迫每次重探测/改 manifest,产品可用性受损。这是反对'只严格、不留口子'的最强现实论据。
- 【'先严格、不建缝'是诱人但错的替代方案】如果为了简单连 CoordinateTransform 挂点都不留(坐标直接用原始像素),那将来加自适应要动租约(Q2 的 Detection 坐标语义)、每个绑定动作、trace schema 三处——正是我们想避免的大 retrofit。反对草案的一种朴素读法('M0 就写死像素,以后再说')在此被否掉:缝必须现在留,只是先恒等。
- 【单个标量 scaling_mode 表达不了 letterbox,M0 数据模型若只留 scale 会锁死将来】底料坑2:letterbox 需要 offset + 视口矩形,不是一个 scale 能表达;底料[锚点+相对坐标]需 2 个分离锚点解 scale+offset。若 M0 只在 manifest 留 scaling_mode 标量,后置模块无法表达 letterbox/视口,等于假留口子。反对'口子留得太浅'。
- 【过度工程风险:多尺度/子像素/金字塔现在做会引入不确定性且无 M0 驱动】底料[分辨率自适应:业界做法/坑]的多尺度扫描、coarse-to-fine、子像素峰值拟合都成本高;子像素插值/浮点峰拟合还会给确定性核心引入非确定源。现在做违反'简洁优先'且无第一条真实任务驱动。替代方案('M0 就上全自适应')应否决,只做归一化那一档。
- 【DPI 校验若只当 mismatch 拒会误伤:§6.3 要求 DPI Awareness 是硬需求】DPI 缩放下 live 客户区像素≠逻辑尺寸,若门粗暴按物理像素比 base 判不等就拒,会把'其实是同一逻辑分辨率、只是系统 DPI 放大'误判为不兼容。门必须先把 DPI 归一到统一坐标系再比,否则严格拒会退化成 Windows DPI 经典 bug 的翻版。这是对'门的实现细节'的反对,需在 M0 就处理 per-monitor-v2。

**裁决建议**

采纳草案主干,并把它精确化为「双层门 + 单一坐标转换缝 + M0 发货即严格闭合」三件套。(1) 门的位置=两层,均在 Lua state 创建之前:Layer A 加载期静态校验 project.toml 声明(base_resolution、scaling_mode、resolution_policy、capabilities、模板集合存在、ROI 落在 base_resolution 内);Layer B run 前 fail-closed 运行期门,查询目标 live client rect / DPI / window_class / exe_sha256,与 compatibility.toml 的 verified 记录比指纹(复用 §6.2 自动降级 unverified),并计算坐标变换。任一不过 → 返回 fail(AutomationErrorKind::UnsupportedResolution 或 TargetCompatibilityUnverified),不进 Lua。(2) 现在就留挂点,但发货时闭合:在 project.toml 引入 resolution_policy 枚举,M0 只允许 strict;数据模型不留标量 scale,而留完整 CoordinateTransform{scale_x, scale_y, offset(viewport 原点), viewport_rect},M0 strict 下恒等(scale=1,offset=0,viewport=full)。所有坐标动作与 find() 产出的 Detection 坐标一律经此 transform 映射,transform 身份并入 Q2 租约与 TargetGeneration。宿主签名(C++ 侧,遵 Result<T>/fail 约定):struct ResolutionGate{ Resolution m_baseResolution; Resolution m_liveResolution; Dpi m_liveDpi; ScalingMode m_scalingMode; ResolutionPolicy m_policy; }; [[nodiscard]] auto evaluateResolutionGate(ResolutionGate const&) -> Result<CoordinateTransform>; —— strict 策略下先按 DPI 归一到逻辑坐标,再要求 live==base(逐轴、遵 scaling_mode)否则 fail;将来加的 uniform_scale 策略只改这个函数体返回真实 scale/offset,Lua 可达面与绑定层零改动。(3) 排序:分辨率自适应作为 strict 之后第一个后置模块,只做'均匀缩放归一化'一档(纯几何、确定性),多尺度/子像素/锚点为更后档,均不进 M0。这样既守住严格后台不静默降级,又用一条恒等缝把'以后能低成本加自适应'兑现,直接回应 ok-script'严格拒太烦'的痛点。

**灵魂约束核对**

确定性:门只在 run 起点读一次 live resolution/DPI(单调、单次读),不参与逐帧;strict 恒等变换与 uniform_scale 都是整数/纯几何映射,给定(base, live, mode)结果唯一。硬约束:M0 及归一化档禁用子像素插值/浮点峰值拟合进入核心坐标路径(底料坑1);若将来引入,必须 pin 算法并把参数写入 trace 与 compatibility 指纹。变换在一次 run 内固定,TargetGeneration 变化(窗口 resize/句柄重建)同时作废租约并强制重新过门,不存在中途悄悄换 scale。可追踪:run 起点强制 emit 一条 ResolutionResolved/RunGateChecked trace 事件,记录 base_res、live_res、dpi、scaling_mode、计算出的 scale_x/scale_y/offset/viewport_rect、以及 compatibility 校验状态+指纹(exe_sha256/window_class),对应底料'trace 记录实际 scale/offset/视口矩形以保可追踪';fail-closed 拒绝也 emit 事件(非静默);后续每个坐标动作 trace 落的是变换后的屏幕坐标,租约携带变换身份供 §6.5 离线复核重演映射。严格后台不静默降级:fail-closed 是本裁决核心——不匹配=显式 run 失败(UnsupportedResolution/TargetCompatibilityUnverified),绝不'先 stretch 再赌能点中';即使将来上自适应,若 live 在声明的 scaling_mode 下不可表达(如宽高比不符而 mode=none)仍失败而非拉伸,对应 Playwright dialog auto-dismiss 反面教材;门在 Lua state 创建前执行,mismatch 目标一个动作都跑不了。核心零游戏分支:base_resolution/ROI/scaling_mode/capabilities 全在 project.toml + compatibility.toml(项目侧,§27 表:坐标转换=框架、基准分辨率+ROI=项目);C++ 门是通用几何+指纹校验,无任何游戏名或专用分支。

**题间依赖**

- Q2(租约):CoordinateTransform 是租约绑定内容的一部分——Detection 坐标已在变换后屏幕空间,§6.3 的 TargetGeneration 递增(窗口 resize/句柄重建)必须同时作废租约并强制重新过门(分辨率可能已变)。Q8 的门与 Q2 的租约共享 generation/transform 身份,不能各判各的。
- Q3(模板/ROI 在 manifest):base_resolution+ROI 与模板同处 project.toml;门在加载期校验模板集合存在且 ROI 落在 base_resolution 内。Q8 门是 Q3 声明的执行/兜底点——Q3 决定声明什么,Q8 决定在哪拒。
- Q1(帧模型):capture() 产帧为 live 分辨率,find() 把 template-space 命中经 transform 反投影回屏幕坐标;'一帧多识别器'模型要求同一帧内 transform 一致应用一次,与 Q1 的显式 capture 边界对齐。
- Q6(interrupts):interrupt 识别器同样跑在变换后坐标,其 handler 的 click 走同一道门/同一 transform;若 Q6 handler 里能再 observe/act,必须复用 run 起点已固定的 transform,不得另算。
- Q10/§9.4(一次一任务、无 daemon):门每 run 起点跑一次(全新 Lua state),契合'一次一个任务',无需常驻重过门。
- ADR-013 存在性(底料[ADR-013 缺失]):Q6 引用的 ADR-013 在 DESIGN v0.4 缺失,但 Q8 引用的 §6.2 确实存在,故 Q8 立论稳;需与 M1 退出契约重措辞联动(底料[里程碑冲突]:'资源引用可在加载时验证'是否涵盖分辨率声明的校验)。

**需开发者拍板**

以下必须开发者拍板,不替你假设:(1) 分辨率自适应优先级——文档'需要开发者补充'里已列此条。我的建议是 M0 strict + 留恒等缝、自适应作后置第一模块;但你第一条真实日常任务是否就跑在非 base 分辨率(带 DPI 缩放的笔记本/多显示器)决定这个模块的紧迫度。(2) 目标游戏的实际 scaling_mode(fit/fill/stretch/none)——卡厄思梦境/鸣潮/NIKKE 的渲染缩放模式是实测事实,决定 M0 数据模型是否真的需要 offset/viewport(若都是 none 且你只跑固定分辨率,offset 档可先留空实现)。(3) DPI 处理策略——§6.3 把 DPI Awareness 列为硬需求;你机器是否恒为 per-monitor-v2?门是否要在 M0 就把 DPI 归一(我建议要,否则严格拒会误伤 DPI 放大的同逻辑分辨率目标)。(4) 标准化的 base 分辨率(1920×1080?你的截图/采集设置)。(5) M0 场景/第一条真实任务(文档 open 列表已列)——决定分辨率自适应是否根本不在首个交付的关键路径上;若首任务恒在 base 分辨率,自适应可安心后置。

---

### Q9. 沙箱:暴露哪些 Lua 标准库,禁 require 后子任务复用怎么办

**草案立场**

锁死沙箱——脚本只拿 bot 句柄 + 纯 Lua 标准库(string/math/table),禁 io/os/package/require/loadfile/load,唯一碰世界的通道是 bot;require 禁了后子任务复用改走受控的 bot:load_subtask("name")。

**正方论据**

- 严格后台的唯一可靠实现是『能力即缺席』:DESIGN §6.4/§3.8/ADR-011 要求绝不静默降级到前台/全局注入。底料明确——Lua 可计算派发调用,静态扫描已声明动作不完整;白名单沙箱把强制点从『扫描动作』变为『危险函数在沙箱里根本不存在』,即使脚本有 bug 或恶意也无法抢焦点/全局注入(SetForegroundWindow/SendInput/os.execute 路径从未绑进 state)。这是相对 NKAS 的核心差异点。
- 确定性铁律(§3.1)要求消除一切不确定输入源。io/os 直接引入墙钟(os.time/clock)、文件、环境变量、os.execute。禁掉它们是确定性前提;呼应 rr 确定性模型(只记录非确定输入)——只有把非确定输入全部堵死或改走宿主受控通道,trace 回放才能重演决策。
- 可追踪(§3.2/§10)要求每次碰世界的调用自动 emit JSONL,脚本『无法忘记埋点』。只有当唯一世界通道是 bot 句柄时,sol2 绑定层才能对每个动作/识别/子任务加载自动生成 span;若开放 io/os,脚本能绕过 trace 直接产生副作用,可追踪性破裂。
- 业界沙箱共识是白名单唯一安全(lua-users SandBoxes、rubenwardy sol3 sandbox):sol::environment(lua,sol::create) 建空 env、env['_G']=env 自引用、白名单逐个拷入、bot 作唯一世界通道——草案方向可精确实现且是主流范式。
- 禁 require 不损失复用:package.preload[name]=<已 load chunk> 惯用法映射到 bot:load_subtask,宿主按名从项目包取源码→lua.load(text)→绑同一沙箱 env→缓存 chunk(来源 lua-l package.preload、ericjmritz)。相比裸 require,它不碰文件系统、能记 trace 归属、能过能力门、可独立测试——更符合确定性+可追踪。
- 动作白名单(§5.5/§15)与 ADR-011 本就规定 Shell/网络/任意路径写不是默认能力;沙箱移除 io/os/package/require/dofile/loadfile 与之天然对齐,SetVariable/Return/StopTask 退化为 Lua 原生(local/return/error),不需要额外绑定。

**反方与风险**

- 『锁死 string/math/table 三纯库就守住确定性』是错觉:底料关键警示——沙箱锁死后隐藏通道仍破坏确定性:math.random 种子全局隐藏且算法平台相关(Redis #95 用每 run reset 种子模型)、pairs/next 顺序依赖表地址(LuaJIT #719 判定『依赖即 bug』)、float→string 走 libc sprintf 平台相关、超越函数(sin/exp/log)libc 相关、collectgarbage/__gc/__mode 引入 GC 非确定。所以『给 math』这句话本身有坑,math.random 必须移除,pairs 依赖顺序必须治理。
- 草案漏了 debug 与 coroutine 表(只列了 io/os/package/require/loadfile/load):底料明确 debug 表 UNSAFE——能读写沙箱外变量,且能反装/卸载 Q5 的取消钩子(与 Q5 硬中断冲突);coroutine.resume(create(fn)) 会让指令计数钩子逃逸、native C 调用不计指令。这两个表必须显式锁死并由宿主独占 coroutine 管理。
- load_subtask 实现不当会重开逃逸口:Scribunto/MediaWiki T49300 越狱根因正是保留的 loader 闭包携带真实 _G。若 bot:load_subtask 把子任务 chunk 绑到稍宽的 env、或半开放 package 表,前功尽弃。必须保证子任务 chunk set_on 到同一沙箱 env,loader 闭包不得携真实 _G。
- 库表若图省事共享引用(env.string=lua.string)会被脚本改 string.* 污染宿主乃至跨 run(rubenwardy 明确要求拷内容而非引用)。草案没写明这一实现约束,是埋雷点。
- bytecode 逃逸未在草案覆盖:底料指出 bytecode 无校验器可破内存安全,必须 lua.load(...,load_mode::text)+查首字节 0x1B 双保险;连 load 本身都不给脚本。草案禁了 load 是对的,但文本模式+首字节校验这层要显式写进实现。
- 『越窄越不方便』是真实成本(与 Q8 ok-script『严格拒太烦』同类):禁 load/string.dump 意味着不能元编程/动态生成分支,按数据表生成逻辑会更啰嗦。可接受(不便 << 不可追踪代价),但需要 bot:load_subtask 和确定性遍历 helper 把最常见的复用/迭代需求接回来。

**裁决建议**

采纳草案方向,但把『纯 Lua 标准库』细化为『逐库消毒后的白名单』,分五步落地(每个 task run 一个全新 lua_State+全新 env,呼应 §9.4 无 daemon):

1) 建空沙箱 env:auto env = sol::environment(lua, sol::create); env[\"_G\"] = env;(自引用,绝不指向真实 _G)。

2) 逐库拷贝内容(非引用)并删危险成员:
  - string:拷贝,移除 string.dump(防 bytecode 泄漏)。
  - table:拷贝,保留 insert/remove/concat/sort/unpack。
  - math:拷贝,移除 math.random/math.randomseed;随机改由 bot:random()(宿主确定性种子,种子入 trace);超越函数保留但在 compatibility 记录平台。
  - 不给:io/os/package/require/dofile/loadfile/load/loadstring/collectgarbage/debug/coroutine/setfenv/getfenv/newproxy。
  - 时间:不给 os.time/clock,提供 bot:now() 返回宿主单调时钟绑定的逻辑时钟(呼应虚拟时钟底料;暂停时不推进)。
  - 遍历:提供 bot:pairs_sorted(t) 确定性遍历;裸 pairs 保留但 lint 警告『依赖顺序即 bug』,平局裁决用声明顺序(§3.1)。

3) 加载脚本强制文本+绑沙箱:加载前查首字节!=0x1B;auto fn = lua.load(src, name, sol::load_mode::text); env.set_on(fn);。

4) bot:load_subtask(\"name\")(替代 require,对接 Q7):宿主按 name 从项目包『已声明子任务集合』取源码(仅项目目录,不碰任意路径)→lua.load(text, \"subtask:\"+name, load_mode::text)→env.set_on(chunk)【同一沙箱 env,loader 闭包不得携真实 _G】→缓存 chunk(package.preload 惯用法);执行自动带 span subtask=name;加载时校验子任务声明能力 ⊆ 任务已声明能力(联动 Q8)。

5) 配套硬中断(绑 Q5):每次 resume 前给协程重装 lua_sethook 指令预算钩子;因脚本无 debug 表无法卸钩子,coroutine 由宿主独占创建,脚本不能自建逃逸协程。

建议把这套沙箱能力面写成新 ADR(很可能就是议程预设的 ADR-013『采用命令式 Lua 任务模型』的配套条款)。

**灵魂约束核对**

确定性:白名单只是第一步,真正守确定性靠库内消毒——移除 math.random(换 bot:random 且种子入 trace)、禁 collectgarbage/debug、提供 pairs_sorted、时间走 bot:now() 单调逻辑时钟(暂停冻结);float 格式化/超越函数标记为平台相关,跨平台位级复现降级为 compatibility 绑定项(接受 rr 式弱确定性:同输入→同决策,非重跑游戏必同果)。可追踪:bot 是唯一世界通道,绑定层对每次动作/识别/子任务加载自动 emit JSONL span;禁 io/os 保证脚本无法产生 trace 之外的副作用;load_subtask 走宿主→子任务加载也进 trace 且带归属。严格后台:能力即缺席——前台/全局注入/os.execute 路径根本不绑进 state,脚本 bug 或恶意都无法降级为前台;这是相对 NKAS 的不可退让差异。核心零游戏分支:沙箱是通用机制,不含任何游戏名判断;bot 暴露的是通用截图/识别/输入/变量/日志能力,C++ 绑定层不得内置游戏专用 helper,游戏差异全在项目侧 Lua+manifest。

**题间依赖**

- Q7(子任务复用)——直接依赖本题:bot:load_subtask 是禁 require 后的替代通道,两题必须一起定 API 签名与 trace 归属;纯 Lua 内部拆函数(不跨文件)天然安全可并存。
- Q5(取消/超时/协程)——双向耦合:debug/coroutine 表必须锁进沙箱,否则脚本卸掉取消钩子逃逸;coroutine 由宿主独占管理,每次 resume 前重装指令预算钩子。
- Q4(错误模型)——脚本 pcall 会吞取消 error(sol2 protected_function 边界),需每 1 指令钩子对抗;pcall 保留给脚本但取消/致命 error 不可被吞。
- Q3(资产声明)——bot.templates.home 的引用面由沙箱决定:脚本只能按名引用,不能 io 读任意 PNG,强化『资产入 manifest』。
- Q8(能力门)——load_subtask 的子任务声明能力必须 ⊆ 任务声明能力,与 Q8 fail-closed 校验联动。
- 横切项:math.random/pairs 顺序/float 格式化/超越函数这类确定性隐藏通道,建议单列一条『确定性 API 契约』或并入新 ADR(与议程引用但 DESIGN v0.4 缺失的 ADR-013 一并澄清)。

**需开发者拍板**

信任模型是最需要拍板的前提:项目包脚本只来自可信作者、还是可能来自第三方?这决定消毒严格度(是否需给 string.rep/format 显式上限防内存炸弹,还是交给内存/指令预算兜底)。其次:(1)是否向脚本暴露随机 bot:random——取决于第一条真实日常任务是否需要随机化行为(如反检测抖动),若需要,种子策略(每 run 固定 vs 记录回放)要定;(2)超越函数(math.sin/exp)在 M0 是否就要求跨平台位级复现,还是接受同平台确定即可,这决定 math 库消毒严格度;(3)M0 是否就需要 load_subtask,还是先单文件任务、子任务复用后置——取决于尚未确定的 M0 场景与第一条真实任务的复杂度。

---

### Q10. 调度：只做"一次一个任务"、无 daemon，还是为托盘常驻留 Engine API 边界

**草案立场**

沿用 DESIGN §9.4——CLI 一次跑一个任务、跑完退出，定时靠 Windows 计划任务；不做队列/优先级/常驻，GUI 调试窗与浮层只是只读消费者。

**正方论据**

- 【确定性·底料直证】一 run = 一个全新 lua_State 是跨运行零泄漏的结构性保证。Lua 移植底料明确：『每个 task run 创建全新 Lua state(隔离+确定性+无跨运行泄漏)，不保留常驻 Lua VM 当调度器』。若把常驻 VM 当调度器，前一 run 的 GC 状态、math.random 隐藏种子、pairs 表地址顺序、全局污染都会漏进下一 run——直接违背确定性铁律(§3.1)。no-daemon 是这条灵魂约束的天然护栏。
- 【可追踪·边界天然对齐】进程边界 = run 边界 = trace 目录边界(§10.1 run-<ts>-NNN/)。一次一进程时每份 trace.jsonl 自包含、run_id 与进程一一对应，§9.3 优雅关闭顺序(广播取消→到安全点→关 Controller→Flush Trace)在单 run 生命周期内可确定性保证 flush；JSONL 信封 run_id 分族(底料)自然落地。daemon 跨 run 复用会让 flush 时机与 run 切分复杂化。
- 【rr 决定论模型佐证】rr 的立身之本是『单线程串行化以消除数据竞争』换取指令级复现(底料 rr 条)。UmbraFlow『一 run 一进程一 VM』是同一哲学：把执行串行化+隔离来赢回可复现性。并行调度器会打破这条串行性，正是命令式 Lua 已经牺牲了加载期可证明性(Q2)之后，最不该再放弃的东西。
- 【业界轻量自动化标准形】SikuliX/AutoHotkey 脚本=进程、定时交给 OS(cron/计划任务)、无常驻 daemon(底料 SikuliX/AHK 条)；Playwright test runner『一 test = 一 fresh context』、并行在 worker/进程层分离。§9.4 与整条赛道的成熟做法一致，不是保守而是主流。
- 【产能与复杂度纪律】§23 风险表明确警告『过早设计的 Engine API 层增加当前不必要的复杂度→开发速度变慢』，缓解是『Engine API 只做薄封装转发』；同表第一风险是『单人+AI 高估任务复杂度』，缓解是『M0 先出可验证 demo』。daemon/队列/优先级/Cron 属于 §2.3『明确移出路线图，不做』，现在写等于把 M0 算力挪去做用不到的基础设施。
- 【API 边界其实已经预留——争议点是伪命题】ADR-008(§24 line795)已决定 M1 就实现同进程 umbraflow-service，CLI 只经它调用；§13.1 的边界目标是『稳定 load_project/start/pause/resume/cancel/query/subscribe 的领域语义』，§13.2/§22 M4 把跨进程 transport/DTO/daemon 推迟到 M4。所以『是否为托盘常驻留 Engine API 边界』的答案已是 Yes 且零额外成本——边界在，只是常驻实现推迟。no-daemon 与将来可常驻并不矛盾。

**反方与风险**

- 【隐式结合是真实陷阱】若把『run 生命周期 == 进程生命周期』写死进 CLI/Runtime(如 Engine 当 process-scoped singleton、以 process exit 作为销毁 run 的唯一路径、lua_close/trace flush 挂在进程退出而非 run 结束),将来给 Engine 加常驻时要大改。这是 casesFor 第 6 条乐观论断的软肋——边界在 API 签名上预留了，但生命周期耦合可能在实现层偷偷违约。守法见 recommendation 层 A 的设计不变式。
- 【进程启动成本】每次 run 都要 luaL_newstate + sol2 sandbox env 构建(Q9)+ 项目加载 + 双层能力协商(§6.2)+ Controller 会话建立。若第一条真实任务是秒级高频轮询，这些固定成本不可忽略。→ 但当前可预见用例(签到/领取/日课)是分~时级，可忽略；真实频率需开发者确认(developerInput)。
- 【跨游戏顺序运行的重连开销】将来『托盘里顺序跑多个游戏的日课』时，逐进程重建 Controller 连接的开销会累积；常驻可保持连接。→ 但这是 M4 GUI 阶段可加『常驻 Engine + 一次一 run』解决的优化(§9.1『复杂度不够再重构』)，现在先取属于早优化。
- 【pause/resume 不可跨进程持久】一次一进程下 pause/resume 只在进程存活期有效，关掉 CLI 则 run 消失，做不到『长时间挂起、隔天恢复』。→ 作为规格接受即可：§9.2 的 pause/resume 是进程内不可分割调用之间的暂停，非持久化；持久挂起是非目标。
- 【替代方案：轻量常驻 supervisor】Engine 常驻但一次只跑一个 run，托盘发火。这是 daemon 的弱化形，能解重连开销又不引入并行。→ 但它属于 M4 GUI 范畴，现在实现违背 §13.2/§23『不做当前用不到的东西』。裁决把它作为 M4 的既定升级路径，而非当前工作。

**裁决建议**

采纳草案，但把问题拆成互相独立的两层写清，避免『进程模型』与『API 边界』被混为一谈：

【层 A — 进程/VM 生命周期（M0-M2 就固化，担保灵魂约束）】CLI 入口 `umbraflow run <project> <task> [--input k=v]...` 为单一 OS 进程，退出码回传 run 结果(0=Completed；区分化非 0 = Failed/Cancelled/LoadFailed)。run 流水线：loadProject → 双层能力协商(Q8, fail-closed，不过则连 lua_State 都不建) → startTask →(内部)luaL_newstate 建 per-run 全新 state → 构 sol2 sandbox env(Q9：仅 bot + string/math/table) → sol::thread::create(lua) 建协程栈(Q5) → resume 循环(取消/暂停/指令预算在 resume 之间检查) → 结束 → lua_close → flush trace → 进程退出。
**关键设计不变式（将来能常驻的钥匙）**：严守『run 生命周期 == lua_State 生命周期』并与『Engine 生命周期』解耦。CLI 里两者恰好同时结束，但代码上 startTask 是『建一个 run 返回 TaskRunHandle』的工厂，run 的销毁(lua_close/flush trace)归 startTask 所有，绝不把 process exit 当作销毁 run 的唯一路径。

【层 B — Engine API 边界（ADR-008 既定，Lua 迁移零改动）】C++ 版 Engine 签名照搬 §13.1 领域语义：
  [[nodiscard]] auto loadProject(std::filesystem::path const&) -> Result<ProjectHandle>;
  [[nodiscard]] auto startTask(TaskRef const&, TaskInputs) -> Result<TaskRunHandle>;
  [[nodiscard]] auto pauseTask(TaskRunId) -> Status;
  [[nodiscard]] auto resumeTask(TaskRunId) -> Status;
  [[nodiscard]] auto cancelTask(TaskRunId) -> Status;
  [[nodiscard]] auto queryTask(TaskRunId) -> Result<TaskStatus>;
  auto subscribeEvents() -> EventSubscription;  // best-effort；lag 后必须 queryTask 取权威快照
Lua 是 startTask 之后的内部实现细节，CLI(及未来 GUI)不感知 Lua(Engine API 表面与 TaskStatus 生命周期不变)。GUI 调试窗/浮层是 subscribeEvents 的只读消费者(草案原样)。

【明文不做】daemon 常驻、run 队列、优先级、Cron/事件自动触发、跨进程 IPC/DTO——全部推到 M4(§13.2/§22 M4/§2.3)；不给 Engine API 加当前用不到的字段/命令(§23『薄封装转发』)。

【写一条 ADR】作为『采用命令式 Lua 任务模型』新 ADR 的一节（或独立 ADR）记录：『调度模型 = 一 run 一个全新 lua_State + no-daemon；API 边界维持 ADR-008；将来常驻 = 常驻 Engine + Controller 会话 + 项目缓存，但仍一次一 run、VM 每 run 用后即弃，故常驻不破坏确定性/可追踪/隔离』。同时把 ADR-001 显式标记为被取代。"

**灵魂约束核对**

确定性：一 run = 一个 luaL_newstate 全新 state，跨 run 零泄漏；math.random 种子、pairs 表地址序、GC 状态、全局污染都随 lua_close 归零(呼应底料确定性隐藏通道消毒条)。单调时钟起点、逻辑预算/generation 计数(Q2/Q1)都是 run 作用域、每 run 重置，绝不跨 run。拒绝常驻 VM 当调度器正是守这条铁律的结构手段。
可追踪：进程边界=run 边界=trace 目录(§10.1)边界，run_id 与进程一一对应，每份 trace.jsonl 自包含；§9.3 关闭顺序在单 run 内确定性 flush，取消/失败/租约失效事件不会因 daemon 复用而漏 flush 或错切。即便 M4 常驻，仍每 run 掘独立 trace 目录，不变式保持。
严格后台：调度层不持有任何输入注入能力，定时由 OS 计划任务(外部)驱动；run 内的严格后台守卫(Q9 沙箱里抢焦点/全局注入 API 根本不存在)与调度模型正交，常驻化也不改。no-daemon 顺带减少『常驻进程在后台自作主张抢前台』的风险面。
核心零游戏分支：调度完全通用，核心只知道『跑一个 run』；游戏差异全在 project/task 参数(外部)。C++ 绑定层/Engine 不含任何游戏名判断或专用调度分支，常驻化也不会引入。"

**题间依赖**

- Q5(协程取消/暂停)：resume 循环是 run 生命周期的心脏；一次一 run 使该循环单线程串行(§9.1)，pause/resume/cancel 三个 Engine API 正是控制这个循环。层 A 的进程模型与 Q5 的 yield/resume 边界必须一致设计。
- Q9(沙箱)：一 run 一全新 lua_State 意味着 sandbox env 每 run 重建——这是跨 run 泄漏防线的另一半，与 Q9 白名单共同守确定性。常驻 VM 会让 Q9 的隔离形同虚设。
- Q1(帧模型)/Q2(租约)：generation 计数器与逻辑预算是 run 作用域、每 run 重置；进程=run 边界让 generation 单调性在 run 内自洽，无跨 run 冲突。
- Q8(能力/兼容门)：双层能力协商(fail-closed)是 startTask 前进程启动流水线的一环，校验不过连 lua_State 都不建——把 §6.2 的门放在进程入口而非 Lua 内。
- Q6(interrupts)/Q7(子任务)：interrupt handler 与子任务都在同一 run/同一 lua_State 内(bot:load_subtask 走宿主受控通道，非跨进程)，其 trace 归属在 run 内闭合；常驻化也把它们锁在 run 作用域，不外溢。
- ADR-001→新 ADR：本裁决要与『采用命令式 Lua』的新 ADR 一并落地，并显式标记 ADR-001(显式状态机)被取代，说明调度模型如何在 Lua 下仍守住可复现。

**需开发者拍板**

1) 第一条真实日常任务的**执行频率**：分~时级则进程启动成本(建 state/sandbox/能力协商/Controller 连接)可忽略，no-daemon 无痛；若存在秒级高频轮询需求，需重新评估(但现行 §2.3 已把常驻列为不做)。2) M4 托盘 GUI 的形态定位：是『顺序发火 UI，仍一次一 run』(本裁决前提，零架构债)，还是『真正的并行调度器』——后者会与 §9.1 并发模型和确定性灵魂约束正面冲突，须重新谈判。此为 product 方向，须开发者拍板。3) 将来跨游戏顺序运行时，是否愿意为『每 run 重建 Controller 连接的开销』付代价，还是届时优先做常驻保持连接——属 M4 优化取舍，非当前决定。这三条我不替开发者假设。"

---

## 4. 框架设计骨架

> 直接搬运 synth（framework-design）。

# 框架设计骨架:命令式 Lua 任务模型

> 本章综合 Q1–Q10 裁决,给出命令式 Lua + sol2 任务模型的**承重结构**:数据流、`bot` API 面、端到端示例、宿主模块边界。
> 四条灵魂约束(确定性 / 可追踪 / 严格后台 / 核心零游戏分支)在每一节以设计不变式形式落地,而非口号。
> 关键事实:失效模型的原语**已在 `modules/domain` 中存在**(`Detection`、`ObservationLease::validate(...)`、`g_defaultMaxActionFrameAge=750ms`、`TargetGeneration`、`AutomationErrorKind`),
> 严格后台守卫**已在 `modules/controller` 中存在**(`g_forbiddenBackgroundApis`、每个坐标动作强制带 `ObservationLease`),
> 识别取消钩子**已在 `modules/vision` 中存在**(`SadSearchPoll`)。Lua 模型是对这些原语的**重新绑定**,不是重写。

---

## 1. 整体数据流:一个观察→动作周期

### 1.1 三方角色

| 角色 | 载体 | 职责 | 绝不做 |
|------|------|------|--------|
| **脚本** | Lua 协程(`sol::thread` 独立栈) | 表达任务决策逻辑:查哪个识别器、命中后做什么动作 | 不碰墙钟/随机/IO、不缓存坐标、不写取消代码、不手造裸坐标 |
| **绑定层** | C++ `bot` userdata(sol2 绑定) | 把每次 observe/act 翻译成 Controller 端口调用;**自动 emit trace**;附着/校验租约;`yield` 出请求 | 不含任何游戏名判断、不含前台/全局注入 API(能力即缺席) |
| **宿主调度** | C++ resume 循环(单线程) | 在 resume 之间检查 取消/暂停/预算;推进逻辑时钟;重装指令钩子;driving Controller 端口 | 不把 process exit 当作销毁 run 的唯一路径 |

三者在**同一 OS 线程**上串行(§9.1、rr 确定性模型)。Lua thread 是 VM 协程非 OS 线程;跨 `std::thread` 访问同一 `lua_State` 被结构性排除。

### 1.2 一个周期的时序

```
脚本(协程)              绑定层(bot)                 宿主调度循环                Controller 端口
   |                        |                            |                          |
   | frame = bot:capture()  |                            |                          |
   |----------------------->| yield{op=capture}          |                          |
   |                        |--------------------------->| check cancel/pause/budget|
   |                        |                            | advance logical_tick     |
   |                        |                            | gen := gen.next()        |------> captureFrame()
   |                        |                            |<-------------------------|<----- Frame{gen,transform}
   |                        |  emit FrameCaptured{seq,    |                          |
   |                        |   elapsed_ns,gen,hash}      |                          |
   |<-----------------------| resume(Frame userdata)     |                          |
   |                        |                            |                          |
   | d = frame:find(home)   |  纯函数求值(vision SAD),   |                          |
   |----------------------->|  emit Recognition{gen,ver, |  (find 不推进帧,不 yield) |
   |<-----------------------|   score,matched}           |                          |
   |                        |  Detection 内挂 lease{gen} |                          |
   |                        |                            |                          |
   | bot:click(d)           |                            |                          |
   |----------------------->| yield{op=click, lease=d.lease}                        |
   |                        |--------------------------->| check cancel/pause       |
   |                        |  emit ActionStarted(before帧)                         |------> deliver(action, lease)
   |                        |                            |                          |  lease.validate(sess,gen,
   |                        |                            |                          |    frameId, now)  ← fencing
   |                        |                            |  gen := gen.next()       |<----- Status ok / StaleObservation
   |                        |  emit ActionCompleted(after)|  作废所有存活 Detection  |
   |<-----------------------| resume(ok)                 |                          |
   |                        |                            |                          |
   | -- 旧 frame/d 现已 stale;下一步必须重新 capture --                            |
```

### 1.3 设计不变式(守灵魂约束的结构手段)

1. **generation 是失效主判据、fail-closed**(Q2 修正一)。`Detection` 携带 `frame_generation`;单调,每次 `capture()` 与每次输入动作后 `TargetGeneration::next()`。校验用 `ObservationLease::validate(...)`,**下沉到最靠近副作用的 Controller `deliver` 层**(Kleppmann fencing:generation 一路带到注入点原子校验),不在 Lua/宿主中间层做"最终"校验。
2. **逻辑预算是超时兜底、不参与"帧是否换了"**(Q2 修正一;EBR 底料)。不读墙钟:`max_action_frame_age`(§8.3 默认 750ms,项目只能调短,复用现存 `clampMaxActionFrameAge`)重定义为逻辑 tick 数;暂停期间逻辑时钟冻结。墙钟 `ts` 仅作 trace 诊断字段,回放按 `seq`/`logical_tick` 驱动。
3. **动作后强制作废 + 强制重 capture**(Q1、§26 规范循环)。`click` 成功 → generation++ → 所有存活 `Detection`/`Frame` 变 stale → 复用即 raise `StaleObservation`,而非静默点击。兜 TOCTOU Type II 只能靠"每次 act 前强制 capture"这条铁律。
4. **脚本无法忘记埋点**(§3.2)。FrameCaptured / Recognition / ActionStarted / ActionCompleted 全由绑定层在调用/抛出的瞬间 emit,先于脚本可能的 `pcall`。脚本吞错也无法从 trace 抹除失败。
5. **危险能力即缺席**(Q9、§6.4)。抢焦点/全局注入/`os.execute` 路径**根本不绑定进 `lua_State`**。脚本即便有 bug 或恶意也降级不了——这是相对 NKAS 的不可退让差异。

---

## 2. `bot` 句柄 Lua API 面草案

### 2.0 沙箱全局环境(Q9)

每个 task run 一个全新 `lua_State` + 全新 `sol::environment`(§9.4 无 daemon)。可达全局**只有**:

| 暴露 | 内容 | 消毒 |
|------|------|------|
| `bot` | 唯一世界通道(下列全部方法) | — |
| `string` | 拷贝(非引用) | 移除 `string.dump` |
| `table` | 拷贝 | 保留 `insert/remove/concat/sort/unpack` |
| `math` | 拷贝 | **移除 `random`/`randomseed`**;超越函数保留但平台性记入 compatibility |
| `pcall`/`error`/`assert`/`select`/`type`/`tostring`/`ipairs`/`pairs` | 保留 | `pairs` 保留但 lint 警告"依赖顺序即 bug",平局裁决用声明顺序(§3.1) |
| **不给** | `io` `os` `package` `require` `dofile` `loadfile` `load` `loadstring` `collectgarbage` `debug` `coroutine` `setfenv`/`getfenv` `newproxy` | 全部逃逸/非确定通道(SandBoxes 四大向量) |

加载三步:`load_mode::text`(挡 bytecode)+ 首字节 `!=0x1B` 双保险 → `env.set_on(chunk)` → `env["_G"]=env`(自引用,绝不指向真实 `_G`)。

**错误层级(贯穿全表,Q4):**
- **Tier A 预期缺席** — 返回 `nil`/`false`,不抛,不算 `AutomationError`(正常控制流)。
- **Tier B 可恢复异常** — 抛结构化 table `error({kind=..., retryable=..., module=..., message=...})`,脚本 MAY `pcall`。
- **Tier C 宿主控制信号** — Cancelled / 全局预算耗尽 / 指令预算超限 / InternalInvariant,**不可被脚本 `pcall` 捕获**(命中后每 1 指令重装钩子再抛,越过任何 `pcall`)。

### 2.1 确定性 helper

| 方法 | 签名 | 语义 | 错误 | trace |
|------|------|------|------|-------|
| `bot:now()` | `-> integer` | 宿主单调逻辑时钟(tick 计数);暂停冻结。**替代** `os.time/os.clock` | — | — |
| `bot:random(m?, n?)` | `-> integer` | 宿主确定性种子 RNG;**替代** `math.random`。种子入 trace 供回放 | — | 种子在 run 起点事件记录 |
| `bot:pairs_sorted(t)` | `-> iterator` | 按 key 排序的确定性遍历。**替代**依赖表地址序的裸 `pairs` | — | — |
| `bot:log(level, msg, fields?)` | `-> nil` | 结构化诊断,经统一 sink → April2 logger + trace | — | `ScriptLog{level,msg,fields}` |
| `bot.input.<key>` | 只读字段 | TaskInput(来自 CLI),边界已强制转 `boolean/integer/string/duration`(§8.5) | 读未声明 key → nil | 在 run 起点快照 |

### 2.2 观察类(Q1)

| 方法 | 签名 | 语义 | 错误 | yield/trace |
|------|------|------|------|-------------|
| `bot:capture()` | `-> Frame` | 取一帧只读 userdata;generation `+1`。**唯一推进帧的调用** | Tier B `CaptureStalled`/`CaptureUnavailable`;Tier C 取消 | **yield 点**;emit `FrameCaptured{seq,elapsed_ns,gen,frame_hash}` |
| `Frame:find(recognizer)` | `-> Detection\|nil` | 对**本帧**单次确定性求值(vision SAD),无重试。命中返回带租约的 `Detection`;未命中 `nil` | **Tier A** 未命中=nil;旧帧再用 → Tier B `StaleObservation` | 不 yield(帧固定);emit `Recognition{gen,recognizer_version,score,matched}` |
| `Frame:match(list)` | `-> label, Detection\|nil` | 多识别器有序决策 helper。严格按**声明顺序**求值、平局按声明顺序裁决(§3.1),把确定性平局关进绑定层 | Tier A 全不命中=`nil,nil` | emit 每识别器 `Recognition` + `CaseMatched{label}` |
| `bot:wait(recognizer, opts)` | `opts={timeout,poll} -> Detection` | 轮询循环,**每轮重 capture**(generation 递增),迭代间 yield;命中即返回 | **Tier B** 超时 → raise `Timeout` | **每轮 yield**(§9.2 取消检查点) |
| `bot:exists(recognizer)` | `-> Detection\|nil` | 单帧一次(内部 capture 一帧)、不抛 | Tier A | yield 一次 |

`recognizer` 只接受 `bot.templates.<name>`(Q3 opaque `RecognizerHandle`);传字符串/临时表 → Tier B `InvalidResource`。

### 2.3 动作类(Q2 / §5.5 白名单)

所有坐标动作:把 `detection.lease` 一路透传到 Controller `deliver` 层原子校验;成功后 generation++ 作废所有存活观察;emit before/act/after 三态。

| 方法 | 签名 | 语义 | 错误 |
|------|------|------|------|
| `bot:click(detection)` | `-> nil` | 单击 detection 中心。租约失效即抛 | Tier B `StaleObservation`(retryable) / `ActionRejected` / `ControllerDisconnected` |
| `bot:long_press(detection, ms)` | `-> nil` | 长按 | 同上 |
| `bot:move_pointer(detection)` | `-> nil` | 移动指针到 detection | 同上 |
| `bot:swipe(from_detection, to_detection)` | `-> nil` | 滑动。**二者必须来自同一帧 generation**,否则 raise `StaleObservation` | 同上 |
| `bot:key_press(keyname)` | `-> nil` | KeyPress(用 `actionGeneration` 校验,无需 detection) | Tier B `ActionRejected` |
| `bot:key_down/up(keyname)` | `-> nil` | KeyDown/KeyUp;宿主在取消/暂停/关闭时 best-effort 补 Up | 同上 |
| `bot:input_text(str)` | `-> nil` | InputText。**trace 默认脱敏**不写原文(§10.3),绑定层强制 | Tier B `ActionRejected` |
| `bot:sleep(ms)` | `-> nil` | Wait 动作;yield;500ms 内可被取消 | Tier C 取消 |
| `bot:capture_artifact(name)` | `-> nil` | CaptureArtifact:**唯一允许的文件写**,只落 Engine 配置的运行目录 | Tier B `InvalidResource`(越界路径) |
| `bot:fail(kind, msg)` | `-> !`(不返回) | 显式判 run 失败(替代状态机 StopTask 的失败态) | 抛映射到 `AutomationErrorKind` |

> `SetVariable/CallTask/Return/StopTask` 退化为 **Lua 原生**:`local`、函数调用、`return`、`error`——不再是绑定动作(§5.5)。正常结束 = 任务函数 `return`。

### 2.4 中断(Q6)

| 方法 | 签名 | 语义 | 错误 |
|------|------|------|------|
| `bot:on(recognizer, handler, opts)` | `opts={max_hits,on_exhausted="fail"\|"stop"} -> InterruptHandle` | 注册任务级 interrupt。宿主在**每次 `capture()` 周期**按注册顺序 first-match 扫描;命中→执行 handler(期间挂起 interrupt 检查、禁重入)→handler 后帧已失效→重抓 drain 到不动点 | 超 `max_hits` → emit `InterruptBudgetExceeded`;`fail` 则 raise、`stop` 则注销并继续 |
| `bot:off(handle)` | `-> nil` | 注销 | — |

handler 只经 `bot` 碰世界(Q9);handler 内动作遵租约;handler 耗时计入触发动作预算。trace 事件族:`InterruptMatched`/`InterruptHandlerStarted`/`Completed`/`Failed`/`InterruptBudgetExceeded`(带 `interrupt_id`+`hit_count`)。

### 2.5 子任务(Q7)

| 方法 | 签名 | 语义 | 错误 |
|------|------|------|------|
| `bot:call_subtask(name, inputs)` | `inputs=table(标量) -> ok, outputs` | 一步式:按 name 从项目包 `scripts/<name>.lua` 内容哈希加载 → `env.set_on` 绑**同一沙箱 env** → 缓存 chunk(`package.preload` 惯用法);显式输入表→显式输出(仅 4 类标量) | 未声明名 → fail-closed `InvalidResource`;环依赖 → `InvalidResource`(v1 禁递归) |
| `bot:load_subtask(name)` | `-> callable` | 返回可调用句柄(惰性) | 同上 |

跨边界只允许 `boolean/integer/string/duration`(§8.5);`Detection`/`Frame` 句柄**禁止跨子任务传递**,检测到 → raise。同 VM 内普通 Lua 函数递归由**运行时调用深度守卫**兜底(阈值待定,见 §5)。trace 自动 emit `subtask_enter`/`subtask_exit` span,子流程 observe/act 带 parent span id(合成状态机 CallTask/Return 等价结构)。

---

## 3. 端到端示例任务脚本

`scripts/farm_dream.lua` —— 体现租约、弹窗 interrupt、超时、子任务:

```lua
-- 项目包: scripts/farm_dream.lua
-- 识别器(bot.templates.*)与基准分辨率在 project.toml 声明,不在此内联(Q3/Q8)

-- 弹窗 interrupt:重连弹窗一出现就点掉,最多 3 次,超限判 run 失败(Q6)
bot:on(bot.templates.reconnect_popup, function(ctx)
    bot:log("info", "reconnect popup handled")
    bot:click(ctx.detection)          -- handler 内动作同样走租约
end, { max_hits = 3, on_exhausted = "fail" })

local rounds = bot.input.rounds       -- TaskInput,边界已强制为 integer

for i = 1, rounds do
    -- wait 内部每轮重 capture + yield;5s 内没等到 Home 就 raise Timeout(Tier B)
    local home = bot:wait(bot.templates.home, { timeout = 5000, poll = 300 })
    bot:click(home)
    -- click 成功后 generation++ ⇒ home 已 stale,复用会 raise StaleObservation

    -- 子任务复用:领取奖励,显式输入输出、独立 trace 归属(Q7)
    local ok, out = bot:call_subtask("collect_rewards", { greedy = true })
    if not ok then
        bot:log("warn", "collect_rewards skipped this round")
    end

    -- 显式 capture 一帧,同帧多 find(Q1 Model B)
    local frame = bot:capture()
    local label, det = frame:match({
        bot.templates.result,          -- 声明顺序即平局裁决顺序(§3.1)
        bot.templates.result_fail,
    })

    if label == "result" then
        bot:click(det)                 -- det 的租约绑定本 frame 的 generation
    elseif label == "result_fail" then
        -- 可恢复:提取标量而非保留 Detection/Rect,跨 step 只传业务标识(§5.3/§8.5)
        bot:log("warn", "round failed", { round = i })
    else
        bot:fail("RecognitionFailed", "neither result screen appeared")
    end

    -- 规范循环:必须重新 capture 才能继续对世界下手(§26)
    local reset = bot:wait(bot.templates.reset, { timeout = 3000 })
    bot:click(reset)
end
-- 正常结束 = return(退化的 StopTask);取消/预算超限是 Tier C,脚本无法拦
```

> 脚本里**没有一行取消/暂停/埋点/时钟代码**:取消由宿主在 `wait`/`capture`/`click` 的 yield 点注入(Q5);trace 由绑定层自动 emit;时间只有 `bot:now()`。纯 Lua 死循环由指令计数钩子 500ms 内硬中断。

---

## 4. 宿主执行引擎的模块边界

### 4.1 更新后的模块图(保持 acyclic,`core` 仍是无 link 依赖的叶子)

```text
core                      (kernel;无 link 依赖;零 Lua、零游戏)
domain      -> core       (Frame/Detection/ObservationLease/TargetGeneration/AutomationError
                           + 新增 IController 端口抽象)
vision      -> core, domain          (SAD 识别;SadSearchPoll 取消钩子)
script      -> core, domain, vision  (新增:sol2 沙箱 + bot 绑定 + 协程调度 + 指令钩子
                                       + interrupt 注册表 + 子任务加载器;私有 link: lua5.4 sol2)
engine      -> core, domain, vision, script  (新增:Engine API + run 生命周期 + 能力/分辨率门
                                               + 项目/manifest 加载 + trace sink 分发)
controller  -> core, domain          (Windows;实现 IController 端口:WGC 截图 + 严格后台注入 + 审计)
entry/cli   -> engine, controller    (组合根:注入具体 Controller,跑一个 run)
```

**关键决策:`IController` 端口放 `domain`**。它只引用 domain 类型(`Frame`/`Detection`/`ObservationLease`/`Point<ClientSpace>`/`AutomationError`),天然属于平台无关领域层。这样 `controller`(实现)与 `script`/`engine`(调用)都只依赖已有的 `domain`,图保持 acyclic,`script` 运行时保持平台无关且可被 Fake Controller 替换(满足"测试确定性、离线"硬约束)。

### 4.2 各模块落地职责

| 模块 / 路径 | 承担的裁决 | 关键内容 |
|-----------|-----------|---------|
| **`modules/domain/.../port/`**(新增) | Q2/Q10 端口 | `class IController`(纯虚):`captureFrame() -> Result<Frame>`、`deliver(Action, ObservationLease) -> Status`、`refreshTarget() -> Result<DeliveryTarget>`。复用现存 `ObservationLease::validate(...)` 作为 deliver 内的 fencing 校验 |
| **`modules/script/.../sandbox/`**(新增) | Q9 | 建空 `sol::environment`、逐库拷贝消毒、`load_mode::text`、`env.set_on`;白名单是唯一策略 |
| **`modules/script/.../bot/`**(新增) | Q1–Q7 | `bot` userdata 绑定;`RecognizerHandle` 只读表;每次 observe/act 自动 emit trace + 附/校验租约 + `yield` 出请求 |
| **`modules/script/.../coroutine/`**(新增) | Q5 | `sol::thread::create` + resume 循环;`status`-first(#883);跨帧存活用 `main_` 引用防 GC;`lua_sethook(LUA_MASKCOUNT,N)` 每次 resume 重装;取消 `std::atomic<bool>` |
| **`modules/script/.../interrupt/`**(新增) | Q6 | 宿主控制的**有序**注册表;每 capture 周期 first-match 扫描;禁重入;drain-to-fixpoint |
| **`modules/script/.../subtask/`**(新增) | Q7 | `package.preload` 惯用法;名字→内容哈希寻址;依赖图检环;调用深度守卫 |
| **`modules/engine/.../lifecycle/`**(新增) | Q10 | `loadProject/startTask/pause/resume/cancel/queryTask/subscribeEvents`(ADR-008 签名不变);**run 生命周期 == lua_State 生命周期**,与 Engine 生命周期解耦;一 run 一 `luaL_newstate`、用后 `lua_close` |
| **`modules/engine/.../gate/`**(新增) | Q8 | 两层门,**均在建 `lua_State` 之前**:Layer A 加载期静态校验 project.toml(base_resolution/scaling_mode/capabilities/模板集合/ROI 落界);Layer B run 前 fail-closed 查 live rect/DPI/exe_sha256 比指纹 → `Result<CoordinateTransform>`。M0 只允许 `resolution_policy=strict`(恒等变换),留 `CoordinateTransform` 完整数据模型(**复用现存 `uf::CoordinateTransform`**)作自适应挂点 |
| **`modules/engine/.../trace/`**(新增) | Q10/§10 | 从 `entry/m0-demo` 提升 `IJsonlSink`/`JsonlLog`/`LogLine` 原型;固定信封 `{ts, run_id, event, data}`;写入即 flush;可选 `prev_hash/curr_hash` 哈希链(失败/租约失效不可事后抹除) |
| **`modules/controller`**(现存,加适配) | §6.4 严格后台 | 实现 `IController`;复用现存 `g_forbiddenBackgroundApis`、`click(...,ObservationLease,...)`、`AuditLog`。deliver 内 `lease.validate(...)` 是最后一道 fencing |
| **Fake Controller**(`tests/` 或 `modules/engine/.../testing/`) | 确定性/离线测试 | `IController` 的确定性实现:喂 `vision/synthetic` 合成帧、把 deliver 记入内存审计缓冲而非 Win32。M0 场景与所有 offline 测试跑在它上面(呼应现存 m0-demo 的 file-queue `input-agent` 精神) |
| **浮层 / GUI 调试窗**(未来,M4,`entry/` 外) | §9.4 | `subscribeEvents` 的**只读消费者**;core/runtime 零 UI 依赖,不感知 Lua |

### 4.3 core 零游戏分支守点(§2.4)

- `core` 保持纯 kernel:不引入 Lua/sol2、不引入任何游戏名。
- `script` 绑定层不得内置任何游戏专用 helper 或弹窗名判断;`bot.templates.*`、interrupt、subtask 都是**通用机制**。
- 所有游戏差异只存在于项目侧:`scripts/*.lua` + `project.toml`/`compatibility.toml` + `assets/templates/`(§11、§27 分工表)。
- 守卫由现存 `scripts/check_modules.py`(强制 `core` 无 link 依赖 + 图 acyclic)+ `check_safety.py` 延伸检查。

---

## 5. 待 grill 敲定(引用对应 Q 的 developerInputNeeded)

以下**不替开发者假设**,必须在 grill 拍板:

| 编号 | 待定项 | 卡点 |
|------|--------|------|
| **Q1** | ① 是否把 raw `frame:find` 暴露给脚本,还是只给 `wait/exists/match` 便利封装、裸 `capture` 藏进宿主(人体工学 vs 控制面)② `max_action_frame_age` 实际值(默认 750ms,按目标帧率定)③ 一次决策典型查几个识别器——验证"同帧多查"效率前提 | 依赖**第一条真实日常任务的决策结构**;文档 132–135 行标注真机现停在角色详情页、非 §26 起点 |
| **Q2** | ① `max_action_frame_age` 改逻辑预算后单位:"N 个 tick" 还是保留"单调 ms + generation 双轨" ② 租约失效是否允许脚本受控重试(可恢复)还是一律判 run 失败 ③ 是否强制"动作后再观察确认"闭环兜 TOCTOU Type II(每动作多一次 capture 成本) | 依赖第一条真实任务里 stale 频率 |
| **Q4** | ① 未捕获 `StaleObservation` 是否绑定层内建小重试 N 次后再判失败,还是立即失败全交脚本 ② 向脚本暴露原生 `pcall` 还是只给受控 `bot:try(fn)`(保证不能捕获 Tier C)③ 各 kind 的 `retryable` 默认值 ④ `SOL_EXCEPTIONS_SAFE_PROPAGATION` 构建标志 | ①③依赖真机实测;④由构建负责人拍 |
| **Q5** | ① 指令预算 `N` 与全局 `max_runtime`/最大 slice 数标定 ② pause 是否 M0 必需还是后置(M0 裸 demo 可能只需 cancel+timeout)③ "500ms 内响应取消"是否 M0 硬指标、是否含纯 Lua 死循环中断 ④ Ctrl-C 与 `Engine.cancel` 是否共用取消路径 ⑤ per-op 分层超时 M0 全需还是子集 | ①依赖第一条真实任务 |
| **Q6** | ① 是否**新写 ADR-013『任务级 interrupts』**并作废对现有 ADR-013 的引用(v0.4 只到 ADR-012)② `max_hits` 默认值 + `on_exhausted` 默认(fail/stop)③ interrupt 仅 run 作用域还是可按 step 启停 ④ handler 能否调子任务、允许多深 ⑤ 命中弹窗能否致命中止主流程 ⑥ 第一条任务具体有哪些弹窗 | ①是**必须先厘清的 ADR 结构问题** |
| **Q7** | ① `§8.5 v1 禁递归`对同 VM 内普通 Lua 函数递归是否强制:加**调用深度守卫**(阈值?)还是带论证放宽 ② 跨文件复用是否 M0/M1 就需要(YAGNI 可推迟)③ 子任务输出:单返回值 vs 多标量表 ④ 子任务命名是否需命名空间/版本标记 | ①是纯 Lua 天然违反、必须拍的点 |
| **Q8** | ① 分辨率自适应优先级(M0 strict + 恒等缝,自适应作后置第一模块 vs 现在就做)② 目标游戏实际 `scaling_mode`(fit/fill/stretch/none)③ DPI 策略是否 M0 就归一 ④ 标准 base 分辨率(1920×1080?)⑤ M0 场景/第一条任务是否恒在 base 分辨率 | 全依赖**目标游戏实测事实 + M0 场景** |
| **Q9** | ① **信任模型**:项目脚本仅可信作者还是可能第三方(决定 `string.rep/format` 是否需显式上限防内存炸弹)② 是否暴露 `bot:random`(反检测抖动需要?种子策略)③ 超越函数 M0 是否要求跨平台位级复现 ④ M0 是否需 `load_subtask` | ①是最需先定的**前提** |
| **Q10** | ① 第一条任务执行频率(时级则进程启动成本可忽略;秒级高频需重评)② M4 托盘 GUI 定位:"顺序发火 UI 仍一次一 run"(零架构债)还是"真正并行调度器"(与 §9.1 冲突,须重谈)③ 将来跨游戏顺序运行是否为"每 run 重建 Controller 连接"付代价 | ②是 **product 方向**,须开发者拍 |

### 5.1 跨 Q 的三条元决策(建议 grill 优先解决)

1. **ADR-013 归属**([底料 ADR-013 缺失]):DESIGN v0.4 §24 只到 ADR-012,不存在 ADR-013。Q6 草案却把它当既有条款引用。**极可能 ADR-013 就是『采用命令式 Lua 任务模型』本身**。必须先确认:是新写这条主 ADR,还是主 ADR 另编号、interrupts 单列。**在澄清前不能把 ADR-013 当权威条款引用。**
2. **ADR-001 被取代的论证**([里程碑冲突]):ADR-001(显式状态机,理由:直观/可验证/易回放)正是被推翻的决策。新 ADR 须论证 Lua 如何重新赢回"可验证/易回放"——回放靠 §ADR-006 Exploratory 帧复核 + 全量 trace 重演决策(rr 弱确定性模型:同输入→同决策,**非重跑游戏必同果**),可验证性是主要牺牲项。
3. **M1 退出契约重措辞**([里程碑冲突]、§22 line723):`所有状态和资源引用可在加载时验证` 与命令式 Lua 直接冲突。建议改为:**"已声明识别/子任务资产在加载期校验存在+哈希;可静态获知的引用 fail-closed;完整可达性/终止性下沉为运行时预算(max_runtime + 指令计数硬中断)+ lint(best-effort)"**。可达性/终止性是**净损失**,须在 grill 显式承认,不假装无损。

---

## 5. 产品能力锚定

> 直接搬运 synth（capability-anchoring）。

# 产品能力锚定

> 基线：DESIGN v0.4（897 行）、TODO 2026-07-20、Lua 任务模型 grill Q1–Q10 裁决。
> 目的：把 TODO §3（框架层）/§4（能力模块）的 10 项能力逐项锚定为**框架核心能力**或**可选能力模块**，绑定四条灵魂约束，划清零游戏分支红线，给出可验证的验收标准与非目标边界。

## 0. 锚定原则与两处必须先澄清的事实

**锚定标准（单一判据）**：一项能力若是**四条灵魂约束的物理载体**，或处在 `capture→find→act→recapture` 任务闭环的**必经环节**上 → 锚为**核心**；若只提供日常工效、调试可视或可安全后置的增强 → 锚为**可选**。据此判定，我对 TODO 的 §3/§4 分类做了一处再分类（见总表脚注）。

**四约束缩写**：**[确]** 确定性（§3.1）｜**[追]** 可观测/可追踪（§3.2、§10.2）｜**[后]** 严格后台不静默降级（§3.8、§6.4、ADR-011）｜**[零]** 核心零游戏分支（§2.4、§22 M2、§27）。

**两处不能抹平的事实**（照实标注，供开发者拍板）：

1. **ADR-013 是幽灵条款**。本文件 §24 只列到 ADR-012。任务语境反复引用的"ADR-001~013"里，ADR-013 在 DESIGN v0.4 中**不存在**。本章所有锚定与验收**不挂靠 ADR-013**；grill 极可能预设它就是待写的《采用命令式 Lua 任务模型》新 ADR——那是 grill 的产出，不是既有权威。
2. **调试 GUI 技术栈自相矛盾**。TODO §4 写 `Dear ImGui + D3D11`，DESIGN §14 写"技术路线倾向 Tauri 2 + TypeScript"。二者不可能同时成立，列为该项的**开发者拍板点**，不替其假设。

### 总表

| # | 能力 | 锚定 | TODO 原位 | 灵魂约束角色 | 零游戏分支红线 |
|---|------|------|-----------|--------------|----------------|
| 1 | 执行引擎 | **核心** | §3 | [确][追][后] 全部约束的运行时载体；承重墙 | **不碰**（绑定层禁内置游戏 helper=守点） |
| 2 | 能力/兼容性门 | **核心** | §3 | [后][确] fail-closed 守门 | **不碰**（门是通用几何+指纹校验） |
| 3 | trace + 关键帧 + 离线回放 | **核心** | §3 | [追][确] 可追踪的物化 | **不碰**（信封通用，无游戏字段） |
| 4 | Fake Controller | **核心** | §3 | [确][追] 脱真机可测的力量倍增器 | **不碰**（脚本化帧序列，通用） |
| 5 | 分辨率自适应 | 可选 | §4 | [确][后] 纯几何、零约束冲突 | **不碰**（base/mode 入项目 compatibility） |
| 6 | OCR | 可选 | §4 | [确]⚠️ 神经档天然对抗确定性 | **风险项**（守点：模板/ROI/白名单入 manifest） |
| 7 | 标注工具 | 可选 | §4 | [追] 上游产出项目资产 | **不碰**（工具通用，产物是项目资产） |
| 8 | HTML 运行报告 | 可选 | §4 | [追] trace 的人读渲染 | **不碰**（离线只读消费 trace） |
| 9 | 实时调试浮层 | 可选※ | §3 | [后] 只读消费者，受后台纪律约束 | **不碰**（消费通用事件流） |
| 10 | 调试 GUI 窗口/托盘 | 可选 | §4 | [追] 只读消费 Engine 事件 | **不碰**（不破坏 Core 无 UI 边界 §3.4） |

> ※ **再分类说明**：TODO 把"实时调试浮层"放 §3 框架层。按锚定单一判据，它是 `subscribeEvents` 的**只读消费者**（与调试 GUI 同构），不在 observe/act 闭环上，不是任务能确定性完成的前提 → 我锚为**可选**。但它保留一条核心属性：一旦实现，就**强制受严格后台红线约束**（不得违反 §3.8），故守点比其它可选项更硬。此为建议再分类，最终归属请开发者确认。

---

## 1. 框架核心能力

### 1.1 执行引擎（TODO §3｜核心·承重墙）

**锚定**：唯一同时承载四条约束的运行时。把 m0-demo 的固定循环抽成"读 Lua 脚本、驱动 observe/act 周期 + 取消/暂停/超时 + 变量/子任务"的通用引擎。

**四约束关系**：**[确]** 每 run 一个全新 `lua_State`（Q10），`math.random`/表地址序/GC/全局污染随 `lua_close` 归零，单调时钟起点与 generation/逻辑预算均 run 作用域重置。**[追]** capture/find/act/取消/预算超限全部由宿主在 resume 之间**单点 emit** JSONL，脚本拿不到 debug/coroutine 无法抑制或伪造。**[后]** 抢焦点/全局注入 API 从不绑进 state（能力即缺席，Q9）；失败=显式任务失败。**[零]** 调度循环/协程/预算/钩子 100% 游戏无关。

**红线判定：不碰。守点**——C++ 绑定层**不得内置任何游戏专用 helper 或分支**，游戏差异只存在于项目侧 Lua + `project.toml`/`compatibility.toml`。

**验收标准（做完 = 全部满足）**：
- 能读一个 Lua 脚本驱动 `capture→find→click→作废旧 Detection→重新 capture` 的 §26 规范循环；
- 取消/暂停/超时经协程 yield/resume + 指令计数 hook 注入：**普通等待 500ms 内响应取消**（成功标准 3/5、§9.2），**纯 Lua `while true` 死循环可被指令预算硬中断**（Q5）；
- 变量强制为 boolean/integer/string/duration 四类型并在边界校验；子任务调用有**深度守卫**（§8.5 v1 禁递归）；
- **一个可重置核心子流程连续 100 次成功**（§22 M1 退出条件、§26 验收）；
- 每 run 全新 `lua_State`，跑完 `lua_close`，无跨 run 泄漏（Q10）；
- **取消不遗留按键、线程或未 flush 的 trace**（§26 验收）。

### 1.2 能力/兼容性门（TODO §3｜核心·fail-closed 守门）

**锚定**：把 §6.2 双层协商放在**进程入口**，是严格后台与确定性的第一道闸。校验不过**连 `lua_State` 都不建**（Q8/Q10）。

**四约束关系**：**[后]** fail-closed 是本项核心——不匹配=显式 run 失败，绝不"先 stretch 再赌能点中"（对照 Playwright dialog auto-dismiss 反面）。**[确]** run 起点读一次 live resolution/DPI（单调、单次），不参与逐帧。**[追]** 强制 emit `RunGateChecked`/`ResolutionResolved`，**拒绝也 emit**（非静默）。**[零]** 门是通用几何 + 指纹校验，无游戏名分支。

**红线判定：不碰。守点**——`base_resolution`/ROI/`scaling_mode`/capabilities 全在项目侧清单，门只做通用校验。

**验收标准**：
- **Layer A（加载期）**静态校验 `project.toml` 声明（base_resolution、scaling_mode、capabilities、模板集合存在、ROI 落在 base 内）；
- **Layer B（run 前）**查 live client rect / DPI / window_class / exe_sha256，与 `compatibility.toml` 的 verified 记录比指纹，指纹变化自动降 `unverified`（§6.2）；
- 任一不过 → 返回结构化 `UnsupportedResolution`/`TargetCompatibilityUnverified`，**不进 Lua**；
- **后台输入失败不自动降级到 SendInput**（§22 M0 退出、ADR-011）；
- 门内计算并固定本次 run 的 `CoordinateTransform`，其身份并入 Q2 租约与 TargetGeneration。

### 1.3 trace + 关键帧 + 离线回放（TODO §3｜核心·可追踪的物化）

**锚定**：可观测性优先（§3.2）的落地形态；回放是 §6.5 Exploratory + ADR-006 的**离线帧复核**，非回归测试。

**四约束关系**：**[追]** 本项即约束本身。**[确]** 事件按 `seq`/`elapsed_ns` 单调、顺序不依赖墙钟；回放按逻辑序号而非墙钟 ts 驱动（否则非确定时序发散）。**[后]** 租约失效/取消/失败作为**独立事件族、写入即 flush**，建议上 prev/curr 哈希链使其不可事后抹除。**[零]** 顶层信封 `{seq, elapsed_ns, event, data}` 通用，schema 版本入信封。

**红线判定：不碰。**

**验收标准**：
- `trace.jsonl` 覆盖 §10.2 事件族（TaskStarted…、FrameCaptured、Recognition*、ActionStarted/Completed/Failed、CaptureStalled、Warning）+ Lua 合成的 step/label span 与 interrupt span（Q1/Q6 等价物）；
- 每条带单调 `seq`/`elapsed_ns` 与适用 TaskRunId/FrameId/ActionId（§10.2）；动作事件带 **before/act/after 三态**（§8.3）与触发它的 Lua 源码行；
- 帧保存策略：迁移帧/动作前帧/错误帧/低置信度帧，**PNG 或 lossless WebP，不得有损**（§10.3）；用过的模板按内容哈希复制到 `resources/`（§10.1）；
- `metadata.json` 记录 §10.1 必需字段（Schema、Engine 版本/commit、Flow 哈希、模板哈希表、后端、目标 exe 路径/版本/哈希、窗口类、interaction policy、墙钟+单调起点）；
- `umbraflow replay` 可跑离线帧复核，**帮助文本明确"不保证完整 Flow 复现、非自动化回归"边界**（§6.5）。

### 1.4 Fake Controller（TODO §3｜核心·力量倍增器）

**锚定**：把开发者从"唯一真机测试者"瓶颈解放出来（TODO 原话）；是 runtime/任务逻辑**脱真机可测**的基础设施（§18.2）。锚为核心是因为没有它，四约束里的 [确][追] 无法被自动化验证。

**四约束关系**：**[确]** 同一脚本化帧序列 → 同一 trace（决策序列 byte-exact 可复现）。**[追]** 用它断言 trace 事件即验证埋点。**[后]** 可脚本化"后台输入失败"分支，验证不降级。**[零]** 脚本化帧/错误序列纯通用。

**红线判定：不碰。**

**验收标准**：
- 能脚本化返回 Frame 序列、注入延迟、Capture/Action 错误、连接状态（§18.2）；
- runtime 可脱真机跑完整 observe/act 闭环并对 trace 断言；
- 覆盖 §18.1 单测清单里可脱机部分：**ObservationLease 过期、TargetGeneration 失效、CaptureStalled、暂停期间预算冻结**；
- 确定性回归：同输入序列多次运行产出一致决策 trace。

---

## 2. 可选能力模块

### 2.1 分辨率自适应（TODO §4｜可选·纯几何增强）

**锚定可选**：M0 严格拒（live==reference 不匹配即拒）已能守约束；自适应是解 ok-script"严格拒太烦"痛点的后置增量。

**四约束关系**：**[确]** 给定 `(base, live, mode)` 结果唯一，纯整数/几何映射。**[后]** 若 live 在声明 mode 下不可表达仍**失败而非拉伸**。**[零]** base/ROI/scaling_mode 全在项目侧（§27 分工：框架管坐标转换，项目管 base+ROI）。**红线判定：不碰。**

**验收标准**：
- `resolution_policy` 枚举就位，**M0 只允许 `strict`（恒等变换）**；
- 数据模型不留裸标量 scale，而留完整 `CoordinateTransform{scale_x, scale_y, offset, viewport_rect}`，strict 下恒等；
- 后置第一档 `uniform_scale`（均匀缩放归一化）**只改 `evaluateResolutionGate` 函数体，Lua 可达面与绑定层零改动**；
- trace 记录实际 scale/offset/viewport_rect；**M0 及归一化档禁止子像素插值进入核心坐标路径**（见 §4 坑）。

### 2.2 OCR（TODO §4｜可选·识别扩展）⚠️ 确定性风险项

**锚定可选**：§7.2 明确"OCR 不在当前阶段实现，真实缺口出现再评估引入 `ort` 或轻量 OCR"。**M0 不引入。**

**四约束关系**：**[确]⚠️** 神经 OCR 天然对抗确定性（浮点非结合、ONNX 不描述算子求和顺序、多线程 reduction、FMA、x87 80-bit）。**[追]** 若引入须记模型哈希/置信度/原始输出供离线复核。**红线判定：风险项**——若为某游戏 HUD 硬编码字段布局即碰红线；**守点**：模板/ROI/字符白名单入项目 manifest（Q3），核心只提供通用 `vision.ocr.*` 原语。

**验收标准**：
- 能力**拆两级**：`vision.ocr.template_digit`（确定性优先）与 `vision.ocr.neural`（后置）；
- `template_digit`：固定字体 HUD 数字，模板/字符切分 + 白名单，**同帧同参数 byte-exact**；
- `neural`（若引入）：强制模型哈希 + 线程配置（`intra_op=1`/`inter_op=1`/`ORT_SEQUENTIAL`/`OMP_NUM_THREADS=1`）写入 `compatibility.toml` 指纹，**任一变化降 `unverified`**；trace 记模型哈希 + 置信度 + 原始输出。

### 2.3 标注工具（TODO §4｜可选·创作工效）

**锚定可选**：截图→框选→出模板+元数据；capture 模式已是上游第一块砖（TODO）。它产出项目资产，不在任务闭环上。**红线判定：不碰**（工具通用，产物是项目资产）。

**四约束关系**：**[追]** 产出的 name/roi/threshold/base_resolution 直接被能力门加载期校验消费（闭环）。

**验收标准**：
- 截图→框选 ROI→输出模板 PNG + 元数据，格式**与 Q3 Layer 1 的 `[[recognizers]]` 声明一致**（name、kind、roi、threshold、grayscale、base_resolution）；
- 产物可被 1.2 能力门**加载期无缝校验**（文件存在 + sha256 + ROI 在 base 内），形成"标注→声明→校验"闭环。

### 2.4 HTML 运行报告（TODO §4｜可选·trace 人读渲染）

**锚定可选**：把 trace 渲染成可点开时间线，纯离线只读消费者。**红线判定：不碰。**

**四约束关系**：**[追]** 是 trace 的消费侧，不产生新副作用；动作 before/act/after 三态可视（§8.3 对齐）。

**验收标准**：
- 读 `trace.jsonl` + `frames/` 渲染可点开时间线；
- **完全离线**，不依赖引擎运行；
- 时间轴按 `seq` 驱动，墙钟 ts 仅作诊断展示；动作三态与关键帧可点开。

### 2.5 实时调试浮层（TODO §3 → 建议再分类为可选·调试增强）

**锚定可选※**：只读消费 `subscribeEvents`，不在 observe/act 闭环上（再分类理由见总表脚注）。但**一旦实现即受严格后台红线硬约束**。**红线判定：不碰**（消费通用事件流）。

**四约束关系**：**[后]** 必须 `WDA_EXCLUDEFROMCAPTURE`（不进截图，避免污染确定性识别）+ `WS_EX_NOACTIVATE`（不抢焦点），即 TODO 原文纪律。

**验收标准**：
- 在游戏窗口上画识别框/状态，**只读消费** Engine 事件，不回写引擎状态；
- **守卫测试：浮层开启时前台 HWND 与系统光标位置不变**（§26）；浮层自身不进 WGC 截图流（`WDA_EXCLUDEFROMCAPTURE`）。

### 2.6 调试 GUI 窗口 / 托盘（TODO §4｜可选·M4）

**锚定可选**：§14 明确推迟到 M4，前置 M0-M2 完成且核心稳定。只读消费 Engine 事件。**红线判定：不碰**（不破坏 Core 无 UI 依赖 §3.4）。

**四约束关系**：**[追]** `subscribeEvents` 是 best-effort，lag 后必须 `queryTask` 取权威快照（§13.1）。

**⚠️ 开发者拍板点**：技术栈——TODO §4 写 `Dear ImGui + D3D11`，DESIGN §14 写"倾向 Tauri 2 + TypeScript"，**二者冲突需裁决**。托盘若仍是"顺序发火 UI、一次一 run"则零架构债；若要真正并行调度器，与 §9.1 并发模型和确定性正面冲突，须重新谈判（Q10）。

**验收标准**：
- 只读消费 Engine 事件，不破坏 §4.1 分层（Core 无 UI 依赖、CLI/GUI 只经 `umbraflow-service`）；
- 前置条件满足（M0-M2 完成 + 核心稳定）后才启动；技术栈冲突先裁决再落地。

---

## 3. 非目标

### 3.1 DESIGN §2.3 已明确移出路线图（"不做，除非未来场景变化"）
- Wasm Component / Agent Process 扩展系统（无第三方开发者）；
- Tauri Studio / 可视化任务编辑器；
- 项目包签名（Ed25519）、SBOM、Registry 分发；
- **Daemon 模式、Scheduler 的 Cron/事件自动触发**（定时用 Windows 计划任务调 CLI，Q10）；
- Python SDK、跨进程 Service API 传输协议（Named Pipe/WebSocket，推到 M4）；
- **OpenCV 依赖**（模板匹配自实现，避免原生库打包复杂度——直接影响 §4 分辨率/OCR 选型）；
- 项目资源变体（多语言、多渠道 UI、**多宽高比**——直接约束分辨率自适应野心）；
- 内核驱动输入、进程注入/Hook、内存读取、绕过反作弊；
- Android/模拟器/ADB/独立 VM/远程桌面兜底；
- 毫秒级竞技操作、自动瞄准等强实时控制。

### 3.2 本次（Lua 迁移阶段）不做 / 后置
- **神经 OCR、多尺度/子像素分辨率自适应**：M0 均不引入（§7.2；见 §4 选型）；
- **实时浮层、调试 GUI**：M0-M2 不做，后置（§14）；
- **待拍板（非硬非目标）**：pause 是否 M0 必需（Q5）、`max_action_frame_age` 实际值（§8.3 默认 750ms）、指令预算 N、StaleObservation 是否内建小重试（Q2/Q4）——这些需"第一条真实日常任务"形态才能定。

### 3.3 灵魂级永不做（ADR-011，不随场景变化）
- **前台输入（`SetForegroundWindow`/`SetFocus`）与全局注入（`SendInput`/`mouse_event`/`keybd_event`/`SetCursorPos`）永不新增**。后台方案不可行即判目标不兼容，直接移出支持范围（如 NIKKE），**绝不改用抢占用户鼠标键盘的兜底**（§2.2、§27、§29）。

---

## 4. OCR 与分辨率自适应选型倾向（依据联网调研 + 底料一手来源）

### 4.1 OCR — 倾向：两级拆分，`template_digit` 优先，神经 OCR 后置且强门控

**排序结论**：`vision.ocr.template_digit`（固定字体 HUD 数字，模板/字符切分 + 白名单，确定性最强、零外部大依赖）→ 第一档；真需要抗锯齿/风格化字体再引 **Tesseract**（C++ 原生、CPU 单线程 + `tessedit_char_whitelist=0123456789` + PSM 7/8）→ 第二档；ONNX/**PaddleOCR** neural → 最后档且强制模型哈希 + 线程配置入 compatibility 指纹。**整体排序低于分辨率自适应**（§7.2："真实缺口出现再评估"，M0 不引入）。

**理由**：
1. **确定性是灵魂约束，神经 OCR 天然对抗它**——浮点非结合、ONNX 标准不描述算子求和顺序、多线程 reduction、FMA、x87 80-bit 中间精度。固定字体 HUD 数字用小模板/小 CNN 往往又快又准。
2. **Tesseract 是更轻的 C++ 原生路径**：~10MB、清晰高对比 95–99%、CPU ~0.77s；裁 ROI + 字符白名单后更快更准。但实操受线程/浮点/版本影响，**未必位级复现** → 若采用须 pin 版本 + CPU 单线程。对固定 HUD，联网证据指出其 detection 阶段对"已知固定位置数字"属**多余开销**。
3. **PaddleOCR C++ 链路重**（Paddle2ONNX→PP-OCRv4/v5 mobile→RapidOCR 骨架，转换须 dynamic shapes 否则结果偏移），对固定 HUD 属过度工程。
4. **门控要求**（若上 neural）：`intra_op=1`/`inter_op=1`/`ORT_SEQUENTIAL`/`OMP_NUM_THREADS=1`，模型哈希 + ORT 版本 + 线程配置写入 `compatibility.toml`，任一变化降 `unverified`；trace 记模型哈希/置信度/原始输出供 §6.5 离线复核。

### 4.2 分辨率自适应 — 倾向：分档递进，M0 严格拒 + 恒等缝，`uniform_scale` 作后置第一档

**排序结论**：M0 严格 `live==reference` 拒（零成本、防缩放误匹配悄悄点错、契合不静默降级）→ 后置第一档 `uniform_scale` 基准分辨率归一化（单一 `scale=live/reference`，均匀缩放最优、成本最低、纯几何确定性）→ 多尺度 scale-space 扫描 / 锚点+相对坐标 / coarse-to-fine 金字塔为更后档。

**理由**：
1. **纯几何变换、确定性、与四约束零冲突**；对应 §27 分工（框架管坐标转换，项目声明 base+ROI）。M0 只留**恒等缝**（strict 恒等 transform），兑现"低成本加自适应"而不破坏"严格拒不静默降级"。
2. **坑决定数据模型**：下采样使子像素细节退化、相关峰塌陷到整数位置，精定位须原生分辨率 + Gaussian 峰值拟合；letterbox 黑边使屏幕百分比坐标失效，锚点须相对视口"先减 offset 再除 scale" → 故数据模型留完整 `CoordinateTransform{scale, offset, viewport}`，**不留裸标量 scale**。
3. **多尺度成本随尺度数线性/二次增长**，对固定客户端属过度；§2.3 已把"多宽高比变体"移出路线图。
4. **不选特征匹配**（SIFT/ORB/Flann 抗大尺度更好）——它引入不确定性 + 需要 OpenCV 依赖，而 OpenCV 已在 §2.3 明确移出。

**Sources**：
- [PaddleOCR vs Tesseract vs IronOCR (Medium, 2026)](https://medium.com/@ahmad.sohail/paddleocr-vs-tesseract-vs-ironocr-picking-an-ocr-engine-for-net-10a24dc2802e)
- [Paddle OCR vs Tesseract (IronOCR)](https://ironsoftware.com/csharp/ocr/blog/compare-to-other-components/paddle-ocr-vs-tesseract/)
- [PaddleOCR vs Tesseract vs EasyOCR (CodeSOTA)](https://www.codesota.com/ocr/paddleocr-vs-tesseract)
- [Technical Analysis of Modern Non-LLM OCR Engines (IntuitionLabs)](https://intuitionlabs.ai/articles/non-llm-ocr-technologies)
- [Multi-scale Template Matching (PyImageSearch)](https://pyimagesearch.com/2015/01/26/multi-scale-template-matching-using-python-opencv/)
- [Performance Tuning for Template Matching (templatematchingpy)](https://templatematchingpy.readthedocs.io/en/latest/guides/performance-tuning/)

---

## 6. Roadmap 草案

> 直接搬运 synth（roadmap）。

# Roadmap 草案 — 从"移植完成"到"Lua 任务模型可跑通"

> 基线:2026-07-20。Rust→C++ 移植完成(domain/vision/controller/m0-demo,8/8 CI 绿)。
> 本章按"命令式 Lua + sol2"重定义现实重排 DESIGN §22(M0–M4),给出最短关键路径,
> 并把"只有开发者知道的输入"造成的阻塞点单列,供 grill 优先解锁。

---

## 0. 两个必须先讲清的现实偏差(写在最前,因为它们改变里程碑语义)

| # | 偏差 | 证据 | 对 Roadmap 的影响 |
|---|------|------|------------------|
| 偏差 A | **首个真实目标游戏不确定**:DESIGN §22/§26 定 M0/M1 = **鸣潮**、M2 = **卡厄斯梦境**;但 grill "需要开发者补充"与真机截图显示**真机当前停在卡厄斯梦境角色详情页** | DESIGN line 707/716/726/827;grill line 130–135 | 决定 S9(真机接入)跑哪个游戏、§26 场景映射到哪个流程。**这是 grill 头号解锁项**,不锁死则 S9 无法起步 |
| 偏差 B | **ADR-013 在 DESIGN v0.4 缺失**(只到 ADR-012),grill 却引用 ADR-013 | DESIGN line 763–812 只列 ADR-001…012 | S0 必须新写一条 ADR"采用命令式 Lua 任务模型",显式标记 **ADR-001(显式状态机)被取代**,并重写 **M1 退出契约 line 723**"所有状态和资源引用可在加载时验证"——该条与命令式 Lua 直接冲突 |

---

## 1. 为什么不能照抄 §22

DESIGN 旧 M1 的承重墙是 **JSONC Flow + 显式状态机 + 加载期证明**(可达性/终止性/资源引用)。
命令式 Lua 把其中三项**净损失**掉:

- **可达性**(无不可达状态)→ 不可判定,降级为 lint(best-effort);
- **终止性**(超时+max_transitions 结构保证)→ 停机问题,降级为**运行时预算**(逻辑时钟 max_runtime + 指令计数硬中断);
- **资源引用静态枚举** / **background_only 禁用动作静态扫描** → 前者靠 manifest 声明+运行时快速失败补偿;后者反而**补偿更强**——采用"能力即缺席",危险 API 根本不绑进 sandbox。

因此**新 M1 的真正承重墙不是"写状态机执行器",而是"绑定层机制性地把这些丢失的加载期证明,重构为运行时强制"**。
Roadmap 的最短关键路径 = 先把这层机制立起来(沙箱→协程→租约→trace),再接真机。

---

## 2. 关键路径总览

**最短关键路径(串行,阻塞"Lua 任务模型可跑通")**:

```
S0 Grill决议+新ADR ──▶ S1 sol2构建接入 ──▶ S2 沙箱+确定性地板 ──▶ S3 协程调度器+取消/预算
                                                                          │
                                          ┌───────────────────────────────┘
                                          ▼
                              S5 绑定层核心(capture/find/click+租约) ──▶ S6 Trace+离线回放
                                                                          │
                                          ★ 到此:"Lua 任务模型可跑通(离线,Fake Controller 上可证明)"
```

**并行分支(不在关键路径,但决定开发是否被真机瓶颈锁死)**:

```
S1 ──▶ S4 Fake Controller ──(喂养)──▶ S5/S6/S7/S8 全部离线可测
S9 门(纯 C++ 几何) 可与 S2–S6 并行开发
```

| 阶段 | 名称 | 对应 DESIGN 里程碑 | 在关键路径? | 被开发者输入阻塞? |
|------|------|-------------------|:-----------:|:-----------------:|
| **S0** | Grill 决议 + 新 ADR + M1 契约重写 | M1 前置 | ✅ | ⚠️ 部分(游戏/任务问题) |
| **S1** | sol2 + Lua 5.4 构建接入(spike) | 基础设施(Rust 未有) | ✅ | ❌ |
| **S2** | 沙箱 + 确定性地板(Q9) | 新 M1 承重墙 | ✅ | ⚠️ 弱(信任模型) |
| **S3** | 协程调度器 + 取消/预算(Q5) | 新 M1 承重墙 | ✅ | ⚠️ 弱(N/SLA 标定) |
| **S4** | **Fake Controller + Fake Frame Source** | §3 新增(Rust 未有) | ➖(倍增器) | ❌ |
| **S5** | 绑定层核心 capture/find/click + 租约(Q1/Q2/Q3/Q4) | 新 M1 核心 | ✅ | ❌(逻辑),⚠️(常数标定) |
| **S6** | Trace JSONL + 离线回放(Q10/§10/ADR-006) | 新 M1 | ✅ | ❌ |
| **S7** | Interrupts + 子任务(Q6/Q7) | 新 M1 增量 | ➖ | ⚠️(是否首交付需要) |
| **S8** | 能力/兼容性/分辨率门(Q8) | 新 M1 前置真机 | ➖ | ⚠️(自适应优先级) |
| **S9** | 真机接入 + 第一条真实任务闭环 | **DESIGN M1 退出** | ✅(真机腿) | 🚫 **硬阻塞** |
| **S10** | 卡厄斯通用性 / NIKKE / GUI | DESIGN M2/M3/M4 | ➖ | 🚫(游戏)/📌(默认不做) |

> ★ "可跑通"里程碑定义:一段 Lua 脚本能在 **Fake Controller** 上确定性地驱动
> `capture→find→click→作废旧帧→重新 capture` 全环,租约 fail-closed、取消/预算生效、
> 全量 trace 可回放。**这个里程碑完全不依赖开发者输入,也不依赖真机**——这是本 Roadmap 的核心红利。

---

## 3. 各阶段详述

### S0 — Grill 决议 + 新 ADR + M1 契约重写
- **目标**:锁死 Q1–Q10 的 API 面与语义;写新 ADR"采用命令式 Lua 任务模型",标记 ADR-001 被取代;重写 M1 退出契约。
- **交付物**:更新后的 DESIGN(新 ADR 段、M1 line 723 改写为"已声明资源加载期校验 + 可静态获知引用被检查;可达性/终止性下沉为运行时预算")、grill 决议纪要。
- **退出标准**:Q1–Q10 每题有裁决;偏差 A/B 有明确结论(首目标游戏、ADR 编号)。
- **依赖**:无(纯设计)。
- **风险**:决议不完整会让 S2–S6 返工;缓解——先冻结**不依赖真实任务**的机制题(Q1/Q2/Q4/Q5/Q9/Q10),把依赖真实任务的**参数题**(常数标定、Q3 Layer3、Q6/Q7 是否首交付)标为"可后置"。
- **开发者阻塞**:⚠️ 机制题草案已可拍;游戏/首任务问题需开发者(见 §5)。

### S1 — sol2 + Lua 5.4 构建接入(spike)
- **目标**:把 Lua 5.4 + sol2 拉进 CMake,一段文本 chunk 能在 `sol::environment` 里跑通,格式/模块/安全门全绿。
- **交付物**:FetchContent(或 vendored)接入、`umbraflow_lua` 模块骨架、`SOL_EXCEPTIONS_SAFE_PROPAGATION` 构建标志、hello-chunk 冒烟测试。
- **退出标准**:`fix_format --check` / `check_modules` / `check_safety` / build / `ctest -L CI` 全绿;新增依赖不破坏现有 8/8。
- **依赖**:无。
- **风险**:sol2 与 C++23/MSVC 的异常传播配置(见底料 Q4 issue #841);缓解——spike 阶段先跑通异常穿越 Lua 边界的最小用例。
- **开发者阻塞**:❌ **立即可做**。

### S2 — 沙箱 + 确定性地板(Q9)
- **目标**:每 run 一个全新 `lua_State` + 空 env;逐库消毒白名单(string 去 `dump`、math 去 `random/randomseed`、table 保留);`_G=env` 自引用;`load_mode::text` 挡 bytecode;指令计数钩子可装;只暴露 `bot` 句柄。
- **交付物**:sandbox 工厂、消毒后的库表拷贝、`bot:now()`(单调逻辑时钟)、`bot:random()`(确定性种子入 trace)、`bot:pairs_sorted()`;逃逸向量回归测试(默认 loader/bytecode/debug/setfenv 四向量各一条)。
- **退出标准**:脚本拿不到 io/os/package/require/load/debug/coroutine;bytecode 被拒;裸 `pairs` 顺序依赖有 lint 警告;跨 run 零泄漏测试通过。
- **依赖**:S1。
- **风险**:遗漏隐藏确定性通道(float 格式化、超越函数、`__gc`);缓解——按底料"确定性隐藏通道消毒"逐项列清单做测试。
- **开发者阻塞**:⚠️ 弱——**信任模型**(脚本是否可能来自第三方)决定是否给 `string.rep/format` 上限防内存炸弹;可先按"可信作者"实现,预留配置。

### S3 — 协程调度器 + 取消/预算(Q5)
- **目标**:`sol::thread` + resume 循环(status-first,#883);`std::atomic<bool>` 取消;双预算(指令钩子 N + 逻辑时钟 max_runtime);finalize 点 best-effort 补 Up + flush trace。
- **交付物**:调度循环、"请求-yield"数据流骨架(world-touching 都在 resume 之间)、pause/resume 冻结逻辑预算、每次 resume 前重装指令钩子。
- **退出标准**:纯 Lua `while true do end` 能在标定 N 内被硬中断;取消是不可被 `pcall` 吞的 raise;取消后无遗留按键/线程/未 flush trace(对齐 §26 验收末条)。
- **依赖**:S1、S2(钩子依赖 debug 表已锁死)。
- **风险**:`pcall` 吞取消 error;缓解——命中后改"每 1 指令一钩再抛"穿透 pcall(底料 PIL 23.2)。
- **开发者阻塞**:⚠️ 弱——指令预算 **N**、500ms 取消 SLA、pause 是否 M0 必需,依赖首任务形态;可先用保守占位值,S9 校准。

### S4 — Fake Controller + Fake Frame Source(**力量倍增器,尽量前置**)
- **目标**:脚本化帧序列(喂 `capture()`)+ 假注入 sink(记录 lease/generation/边界校验结果,不碰真机),让整条绑定层脱真机可确定性测试。
- **交付物**:`FakeController`(可编排"第 N 帧返回模板命中/未命中/stall")、`FakeFrameSource`、注入审计记录、与 S5 绑定层对接的注入接口抽象。
- **退出标准**:能用一段脚本化帧序列驱动 S5 的全部语义(命中/未命中/stale/generation bump/interrupt/cancel)且结果可复现;无需真机、无需 UAC。
- **依赖**:S1(接口约定);与 controller 模块的注入抽象对齐。
- **风险**:Fake 与真机行为漂移;缓解——真机接入(S9)时用同一批脚本序列做"Fake vs 真机"对照。
- **开发者阻塞**:❌ **立即可做**。
- **为什么排这么前 → 见 §4 专章**。

### S5 — 绑定层核心:capture/find/click + 租约(Q1/Q2/Q3/Q4)
- **目标**:落地 Q1 Model B(显式 capture + 同帧多 find)、Q2 租约(generation 主判据 fail-closed + 逻辑预算兜底,校验下沉注入层)、Q3 manifest 只读句柄表 `bot.templates`、Q4 三层错误模型。
- **交付物**:`bot:capture()→Frame`(只读 userdata,generation++,emit FrameCaptured)、`Frame:find(recognizer)→Detection|nil`、`Frame:match({...})`(声明顺序裁决)、`bot:click/swipe/...`(注入层原子校验 generation→陈旧即 raise StaleObservation→成功 bump generation 作废旧 Detection)、`bot:wait/exists`(SikuliX 三态,内部 yield)、Detection 短命 userdata、三层错误(Tier A 返回值 / Tier B 结构化抛 / Tier C 不可捕获)。
- **退出标准**:在 Fake Controller 上:旧帧再 find / 旧 Detection 再 click 必抛 StaleObservation;`max_action_frame_age` 超龄拒投递;`bot.templates` 只读、`__newindex` raise;每次 find/click 自动 emit 事件。
- **依赖**:S2、S3、S4;Q1↔Q2 互为前提(帧 generation 从 capture 来、租约绑 generation)。
- **风险**:租约校验层放错位置留 TOCTOU 残窗(底料 Kleppmann/arxiv 2603.00476);缓解——generation 一路带到最靠近注入处校验,并强制"动作后重新 capture"闭环兜 Type II。
- **开发者阻塞**:❌ 机制可全实现;⚠️ `max_action_frame_age` 默认值、各 kind `retryable` 默认、是否内建 stale 小重试,需首任务实测校准。

### S6 — Trace JSONL + 离线回放(Q10/§10/ADR-006)
- **目标**:固定信封 `{ts, run_id, event, data}` + 事件族(run 边界/observe/act/interrupt/lease-fail/cancel)、写入即 flush、可选 prev/curr 哈希链、按 logical_seq 驱动回放(墙钟仅诊断)。
- **交付物**:JSONL writer、事件 schema(带版本号)、动作 before/act/after 三态 + 触发 Lua 源码行、实际用过模板按哈希复制到 `resources/`、离线回放器(Exploratory 帧复核)。
- **退出标准**:一次 Fake run 产出自包含 `trace.jsonl`;回放按逻辑序号重演决策一致;取消/失败/lease-fail 事件不可事后抹除(哈希链)。
- **依赖**:S5(事件由绑定层 emit);S3(cancel/flush 时序)。
- **风险**:trace 量膨胀(底料 Robot Framework 教训);缓解——默认只存迁移帧/动作前帧/错误帧/低置信度帧,PNG/lossless,InputText 默认脱敏。
- **开发者阻塞**:❌ **立即可做**。★ **到此"Lua 任务模型可跑通(离线)"达成**。

### S7 — Interrupts + 子任务(Q6/Q7)
- **目标**:`bot:on(recognizer, handler, {max_hits, on_exhausted})` 同步/周期边界/drain-to-fixpoint/显式失败;`bot:load_subtask(name)` 走 `package.preload` 受控通道(禁 require,同一沙箱 env,内容哈希寻址);in-VM 递归深度守卫(§8.5 v1 禁递归)。
- **交付物**:有序 interrupt 注册表 + 新事件族(InterruptMatched/Handler*/BudgetExceeded)、子任务加载图(环检测 fail-closed)、跨边界只允许 4 类标量、Detection/Frame 禁跨边界。
- **退出标准**:Fake 上弹窗被 drain 到不动点;max_hits 耗尽显式 raise;子任务能力 ⊆ 父任务声明能力;递归超深 raise RecursionLimit。
- **依赖**:S5、S6、S2(沙箱)。
- **风险**:handler 重入/纯 Lua 死循环;缓解——handler 期间挂起 interrupt 检查 + 指令钩子兜底。
- **开发者阻塞**:⚠️ 是否首交付需要,取决于首任务是否有弹窗/是否跨文件复用子流程——**可按 YAGNI 后置**。

### S8 — 能力/兼容性/分辨率门(Q8)
- **目标**:双层门(Layer A 加载期静态校验 project.toml 声明;Layer B run 前 fail-closed 查 live rect/DPI/window_class/exe_sha256 比指纹),均在 `lua_State` 创建前;单一坐标转换缝 `CoordinateTransform{scale_x,scale_y,offset,viewport_rect}`,M0 恒等。
- **交付物**:`evaluateResolutionGate() -> Result<CoordinateTransform>`(strict 策略先 DPI 归一再要求 live==base)、`resolution_policy` 枚举(M0 仅 strict)、run 起点 emit ResolutionResolved 事件、mismatch 显式 fail(UnsupportedResolution/TargetCompatibilityUnverified)。
- **退出标准**:非 base 分辨率/未 verified 目标连 `lua_State` 都不建、显式失败;strict 下 transform 恒等;fail-closed 也 emit 事件(非静默)。
- **依赖**:纯 C++ 几何,可与 S2–S6 并行;真机腿(S9)前必须就位。
- **风险**:DPI 逻辑/物理坐标混用(底料 Windows DPI 经典 bug);缓解——门先按 DPI 归一到逻辑坐标。
- **开发者阻塞**:⚠️ **门本身立即可做**;**分辨率自适应模块**(strict 之后的均匀缩放归一化档)是否现在做,是 grill 解锁项(见 §5)。

### S9 — 真机接入 + 第一条真实任务闭环(= DESIGN M1 退出)
- **目标**:把 m0-demo 的真实 WGC/PostMessage controller 接到同一绑定层背后,跑通**第一条真实日常任务**,过 100 连环 + 7~14 运行日 soak。
- **交付物**:真机 controller 适配、鸣潮**或**卡厄斯梦境项目包(project.toml/compatibility.toml/assets)、第一条真实任务脚本、真机 vs Fake 对照报告。
- **退出标准**(对齐 DESIGN M1 line 719–724 改写版):可重置子流程连续 100 次成功;完整日常任务 7~14 日无框架级失败;**已声明资源加载期校验通过 + 可静态获知引用被检查**;CLI 全经 Engine API。
- **依赖**:S5–S8 全部;**真机 UAC 提权一次**(TODO §1,AI 无法自我提权);S0 偏差 A 结论。
- **风险**:首任务表达力不足暴露 API 缺口→返工;缓解——Fake 上已跑通全环,真机主要暴露的是**参数标定**与**目标兼容性**,非机制缺口。
- **开发者阻塞**:🚫 **硬阻塞**——首目标游戏、首任务、§26 场景映射、UAC 提权,全部只有开发者能给。

### S10 — 卡厄斯通用性 / NIKKE / GUI(DESIGN M2/M3/M4)
- **目标**:M2 用第二个游戏验证"核心零游戏分支";M3 NIKKE 默认不执行的技术探测;M4 GUI 只读消费 Engine 事件。
- **退出标准/依赖/风险**:基本沿用 DESIGN §22,不因 Lua 迁移改变;**若 S9 已跑卡厄斯,则 M2 改用鸣潮或反之**(取决于偏差 A 结论)。
- **开发者阻塞**:🚫 游戏选择;📌 M3/M4 本就"默认不做/推迟"。

---

## 4. 专章:Fake Controller 为什么必须前置到 S4

**结论:Fake Controller 排在协程调度器(S3)之后、绑定层核心(S5)之前/并行,是本 Roadmap 杠杆最高的一步。**

- **它把串行依赖变成并行依赖**:没有它,绑定层的每一条语义(租约失效、generation bump、stale、interrupt drain、cancel 500ms)都只能在**唯一真机**上验证——而真机需要 UAC 提权、需要开发者在场、且当前连"跑哪个游戏/哪个任务"都未定(偏差 A + §5)。有它,S5/S6/S7/S8 **全部脱真机确定性可测**。
- **它解耦了"机制开发"与"开发者输入阻塞"**:S9 被硬阻塞的那些问题(首任务、首游戏、场景映射),**不阻塞 S4→S8**。开发者去 grill 解锁真实任务的同时,绑定层可以在 Fake 上一路建到"可跑通"。
- **它是确定性/回放的天然试验台**:脚本化帧序列 = 底料里 rr/record-replay 的"被记录的外部输入",让"同输入→同决策"这条弱确定性可以在 CI 里被钉死,而不必重跑游戏。
- **成本低**:纯 C++ infra,无外部依赖,不碰焦点/注入,不需要提权。
- **唯一代价**:Fake 与真机行为可能漂移——用 S9 的"同一批脚本序列 Fake vs 真机对照"消解。

> 一句话:**Fake Controller 是"把开发者从唯一真机测试者解放"的那把钥匙,越早越省。**

---

## 5. 被"只有开发者知道的输入"阻塞的阶段(grill 必须优先解锁)

| 解锁项(grill 头等) | 阻塞的阶段 | 不解锁的后果 |
|--------------------|-----------|-------------|
| **偏差 A:首个真实目标游戏 = 鸣潮(DESIGN)还是卡厄斯梦境(真机现状)?§26"Home→点击→等 Result"映射到哪个游戏内流程?** | S9、S10 | 真机腿无法起步;项目包无法建;M2 对象连带待定 |
| **第一条真实日常任务是什么**(登录/签到/领取/菜单导航/画面决策?有无弹窗?是否跨文件复用子流程?) | S9 定形;S5 常数校准;S7 是否首交付 | API 表达力是否足够无法验证;Q3 Layer3 参数化识别、Q6 interrupts、Q7 子任务是否 M0 需要全悬空 |
| **分辨率自适应优先级**:现在做均匀缩放归一化档,还是先严格拒、后补模块? | S8 自适应模块范围 | S8 的"门"可做,但"自适应模块"是否进首交付无法定;首任务是否跑在带 DPI 缩放的非 base 分辨率决定其紧迫度 |
| **信任模型**(Q9):脚本仅可信作者,还是可能第三方? | S2 消毒严格度 | 是否需给 string.rep/format 上限防内存炸弹待定(可先按可信作者做) |
| **常数标定**:`max_action_frame_age`(§8.3 默认 750ms)、指令预算 N + 500ms 取消 SLA、各 kind `retryable` 默认、pause 是否 M0 必需 | S3/S5 最终值 | 机制可先跑保守占位值,真机 soak(S9)前必须给真实值 |
| **偏差 B:ADR-013 = "采用命令式 Lua 任务模型"确认 + 编号** | S0 | 新 ADR 挂不上号;ADR-001 被取代的论证无处落 |

---

## 6. 立即可做 vs 必须先 grill 解锁

### ✅ 立即可做(不依赖开发者输入,今天就能开工)

1. **S1 sol2 + Lua 5.4 构建接入**:FetchContent 接入、hello-chunk、`SOL_EXCEPTIONS_SAFE_PROPAGATION`、CI 保持 8/8。
2. **S2 沙箱骨架**:空 env + 逐库消毒白名单 + `load_mode::text` + 指令钩子可装 + 四逃逸向量回归测试(草案已决,按可信作者实现)。
3. **S3 协程调度器骨架**:`sol::thread` + resume 循环 + atomic 取消 + 双预算 + 每次 resume 重装钩子(用保守占位常数)。
4. **S4 Fake Controller + Fake Frame Source**:最高杠杆,纯 infra,喂养 S5–S8 全部离线测试。
5. **S6 Trace JSONL writer**:固定信封 + 事件族 + flush-on-write + 哈希链(schema 约定已决)。
6. **S8 的"门"(纯 C++)**:`evaluateResolutionGate()` + strict 恒等 `CoordinateTransform` + ResolutionResolved 事件。
7. **文书**:重写 M1 退出契约 line 723;起草新 ADR"采用命令式 Lua 任务模型"(作为提案,待开发者拍板批准)、标记 ADR-001 被取代;把 Q1/Q2/Q4/Q5/Q9/Q10 的机制决议冻结成规格。
8. **确定性隐藏通道清单化**:把 float 格式化/超越函数/`__gc`/pairs 顺序逐项列成测试点(底料已给)。

> 这 8 项能把关键路径推进到 **S6 完成 = "Lua 任务模型可跑通(离线)"**,全程不碰真机、不需 UAC、不需开发者拍板。

### 🚫 必须先 grill 解锁(开发者输入是硬前置)

1. **首个真实目标游戏**(鸣潮 vs 卡厄斯梦境)+ §26 场景映射 → 解锁 S9/S10。
2. **第一条真实日常任务形态** → 解锁 S9 定形、S7 是否首交付、Q3 Layer3 是否 M0。
3. **分辨率自适应优先级** → 解锁 S8 自适应模块是否进首交付。
4. **真机 UAC 提权一次**(TODO §1,AI 无法自我提权) → 解锁一切真机验证。
5. **常数标定值**(max_action_frame_age / N / 取消 SLA / retryable / pause) → S9 soak 前必须落定。
6. **信任模型 + ADR-013 编号确认** → 分别定 S2 消毒强度、S0 ADR 结构。

---

## 7. 一句话收尾

**最短关键路径 = S0→S1→S2→S3→S5→S6,终点是"Lua 任务模型在 Fake Controller 上可跑通",且这条路径完全不被开发者输入阻塞。**
开发者输入只阻塞**真机腿(S9)**;因此 grill 应把"首游戏 / 首任务 / 分辨率优先级 / UAC"作为头等解锁项,让真机接入与离线绑定层开发**并行推进**——而 **Fake Controller(S4)是让这种并行成立的那把钥匙**。

---

## 7. 完整性审查

> critique agent 对整份裁决包的交叉审查，分四小节。

### 7.1 内部矛盾（contradictions）

- 【双 generation 混淆】DESIGN §5.4/§6.3 有两个计数器:FrameId(每 capture 单调,高频)与 TargetGeneration(窗口句柄/尺寸/进程变化才++,低频);ADR-004 lease 绑 {target_generation, frame_id}。Q2 修正一把 fencing 主判据命名为'frame_generation,每 capture/每输入动作+1'(实为 FrameId 语义),Q5 却'resume 时 bump target_generation',Q8 用 TargetGeneration 触发重新过门。全程用'generation'一个词混指两个计数器,注入层'原子校验 generation'没有唯一所指。fencing 到底 fence 哪个、lease 字段如何映射 ADR-004,必须先定。
- 【max_action_frame_age 墙钟↔逻辑tick 自相矛盾】Q1 用 max_action_frame_age 明确要防'脚本在长纯-Lua 计算后拿旧帧下手';Q2 修正一把它从 §5.4 的 750ms 墙钟改成'逻辑 tick(仅 capture/tick 事件递增)'。但纯 Lua 长计算期间无 capture→逻辑 tick 不推进→age 不增长→逻辑 tick 方案恰好放行 Q1 要拦的场景。同一参数在 Q1(物理时间语义)与 Q2(逻辑时钟语义)下结论相反。
- 【Timeout kind 在 Tier B/Tier C 碰撞】Q4 示例 bot:wait 超时抛 {kind='Timeout',retryable=false}(Tier B,可 pcall);Q5 又把全局 max_runtime/指令预算耗尽映射到 AutomationErrorKind::Timeout(Tier C,不可 pcall)。DESIGN §5.6 只有一个 Timeout。脚本 pcall 到 Timeout table 无法区分'wait 超时(可重试)'与'预算耗尽(必须死)',trace 也难分流。
- 【回放能力过度承诺 vs ADR-006/§0】Q2、S6、骨架多处称'回放按 logical_seq 重演决策一致''全量 trace 重演决策',但 DESIGN §0 摘要第2条与 ADR-006 明确'不承诺完整任务确定性重放,只做 Exploratory 帧复核'。命令式 Lua 不保存每帧且决策依赖 bot:now/bot:random/中间态,'重演决策'越过了已确立的边界,是对'可追踪'的夸大。
- 【StaleObservation retryable 一处已决一处未决】Q4 抛出示例已硬编码 {kind='StaleObservation', retryable=true} 且'host 提供受控小重试';Q2 developerInputNeeded 又把'租约失效是否允许受控重试还是一律判 run 失败'列为开发者拍板项。同一问题裁决包内部一处当已决、一处当未决。
- 【Q1 raw find 暴露:未决 vs 示例当已决】Q1 developerInputNeeded 明列'是否把 raw frame:find 暴露给脚本还是只给 wait/exists/match 便利封装'为待拍板;但骨架 §3 端到端示例直接使用 frame:find/frame:match。一边列为 open,一边在权威示例里当已决用。
- 【capture() 不再是纯观察 vs 1.2 时序图】Q1 不变式称'capture 是唯一推进帧的调用、find 无副作用、capture 不 deliver';Q6 却让宿主在'每次 bot:capture() 周期'内部扫描 interrupt 并执行 handler 的 click(bump generation、drain-to-fixpoint)。于是脚本调用的 capture() 内部可能产生脚本不可见的输入动作,与骨架 1.2 时序图'capture 只 captureFrame() 不 deliver'冲突。

### 7.2 缺口（gaps）

- 【Frame 8MiB 生命周期无界】Q1 每 capture generation++、bot:wait 每轮重 capture(一次 wait 可抓十余帧),Frame userdata 由 Lua GC 管、各持 Arc<FrameBuffer> 8MiB。命令式 Lua 下脚本可持有任意多 frame 变量(已 stale 但未 GC),与 §3.6'缓存有界'、§19'只保留最新帧+活跃帧引用'冲突。决策包未定 Frame 数量上限/stale frame 强制释放机制。
- 【沙箱白名单遗漏一批基础函数】Q9 表未列 setmetatable/getmetatable/rawget/rawset/rawequal/rawlen/next/tonumber。其中 setmetatable 可注册 __gc/__index/__tostring 元方法——__gc 触发时机随 GC 不确定(非确定副作用)、__index 是逃逸向量、tostring(table) 暴露地址。每个都需显式判定,白名单表当前不完整。
- 【Lua 5.4 integer/float 子类型边界未定】变量/跨子任务标量限 boolean/integer/string/duration,但 Lua 5.4 number 有 integer 与 float 两子类型,3/2=1.5、算术易意外产生 float;bot:now() 返回 integer。float 参与决策比较引入平台格式化/精度非确定。决策包未定 number 子类型在边界(TaskInput/子任务/变量)如何归一或拒绝。
- 【capabilities 声明与脚本实际动作的加载期一致性是未列出的净损失】§8.6 加载期校验'background_only 项目未引用被禁止动作'依赖对动作的静态扫描;命令式 Lua 下脚本用不用 key_press/input_text 是运行时才知。Roadmap 只把可达性/终止性列为净损失,漏了'能力集合 vs 实际动作一致性'这项同样退化为运行时的净损失。Q7'不从子任务源码推断能力'加剧此缺口。
- 【pause 后置与 Q2 resume 作废机制的依赖未协调】Q5 把'pause 是否 M0 必需'列为待定;但 Q2'resume 作废观察'、Q5'resume bump target_generation'依赖 pause/resume 存在。若 M0 砍 pause,Q2/Q5 哪些机制同步后置、generation-bump-on-resume 是否还需要,决策包未给出裁剪边界。
- 【swipe/固定偏移动作表达力】骨架仅允许 bot:swipe(from_detection, to_detection) 且要求同帧 generation,无法表达'从 A 点滑动固定方向/距离到无识别器的目标点'。同理无坐标偏移点击。依赖第一条真实任务是否需要,但当前 API 面对这类常见手势是空的。
- 【wait 内部 interrupt 失败的 surface 语义未定】bot:wait 每轮 capture 都触发 Q6 interrupt drain;若 handler 耗尽 max_hits 抛 InterruptBudgetExceeded,wait 会被该错误打断而非返回 Timeout。脚本 pcall(bot:wait) 捕获到的错误类型因 interrupt 变得不可预测,决策包未说明 wait 内 interrupt 失败如何 surface。
- 【trace 事件流 vs 落盘 trace vs subscribeEvents 三者关系】Q10 保留 subscribeEvents(best-effort,lag 后 queryTask),trace 是绑定层 emit 到 JSONL sink,logging decision 提'统一 sink 分发'。实时订阅流与落盘 trace 是否同源、一次一 run 下 queryTask 返回何种权威快照、TaskStatus 生命周期,未闭合。
- 【bot:capture_artifact 路径/命名安全规则未定】骨架称其为'唯一允许的文件写',写入 Engine 运行目录,提了 InvalidResource(越界路径) 但未给 name 的校验规则(路径穿越 ../、命名冲突、覆盖)。§15 要求'不接受 Flow 指定任意路径'。
- 【元 gap:无一条真实任务却设计了完整 API 面】Q3 Layer3 参数化识别、Q6 interrupt drain-to-fixpoint、Q7 子任务加载图+环检测、哈希链等均为 speculative,YAGNI 风险高;所有效率前提/表达力/常数标定都依赖'第一条真实日常任务'与 M0 场景(决策包自身反复标注未知)。
- 【ADR-013 幽灵条款与 M1 契约重写尚未落地】DESIGN v0.4 §24 只到 ADR-012;Q6 草案把 ADR-013 当既有条款引用。新 ADR'采用命令式 Lua 任务模型'、ADR-001 被取代论证、M1 line723'所有状态和资源引用可在加载时验证'的改写,均为 grill 产出、当前未写入 DESIGN。

### 7.3 约束违背 / 需盯（constraintViolations）

- 【确定性】Q9 保留 math 超越函数(sin/exp 等)'平台性记入 compatibility',等于承认同一脚本跨平台可能产生不同决策,与 §3.1'相同帧/识别器版本/参数产生相同结果'的确定性铁律冲突。裁决自认降级为'rr 式弱确定性(同平台确定)'——这是对灵魂约束的显式让步,需开发者明确批准,不能默认。
- 【可追踪(过度承诺)】'回放重演决策一致'与 ADR-006/§0'不承诺完整任务确定性重放'冲突(见 contradictions)。把可追踪的实际能力(离线帧复核)表述成了'决策级重演',会误导 M1 验收标准。
- 【确定性(外部事件)】Q5 指令钩子'同输入→同计数→同触发点'成立,但取消是 Ctrl-C atomic 异步事件,命中的指令位置取决于 OS 调度时刻→含取消的 run 无法确定性复现到取消点之后。裁决按 rr'被记录的外部事件'处理可接受,但这是弱确定性,需在验收标准里明示,不可宣称'含取消 run 可完全复现'。
- 【简洁优先(元原则)/可追踪】trace 哈希链'不可事后抹除'是裁决新增(DESIGN 从未要求)。唯一用户=作者本人、本地磁盘文件,哈希链只防单条抹除、不防整体重写,防护价值对自用工具存疑,与 §3.7'轻量、不建兼容性基础设施'及 CLAUDE.md'简洁优先/最小影响'张力明显,疑似过度工程。
- 【确定性(隐藏通道)】__gc 元方法在 GC 触发时随时机运行(collectgarbage 虽禁但自动 GC 仍跑),若 setmetatable 在白名单则脚本可注册 __gc 产生非确定副作用;Lua 5.4 integer/float 隐式转换与 float 格式化也是未消毒通道。Q9'确定性地板'尚未覆盖这批通道。
- 【严格后台(需盯,非确凿)】Q6 让 capture() 内部自动执行 interrupt handler 的 click——虽走 background_only 注入路径不违反后台能力约束,但产生脚本不可见的输入动作,审计/归因必须由 InterruptMatched span 严格覆盖,否则'可追踪'出现盲区。列此为监控点。

### 7.4 grill 应追问的靶点（grillTargets）

- 双 generation 计数器语义:先画清 FrameId(每 capture,高频)与 TargetGeneration(窗口事件,低频)两个计数器,定死注入层 fencing fence 哪个、lease 字段如何映射 ADR-004、resume/过门各 bump 哪个。这是 S5 地基,阻塞其余一切绑定层裁决。
- max_action_frame_age 度量:墙钟 ms(守 §8.3 物理 liveness、能拦纯 Lua 长算后旧帧)还是逻辑 tick(暂停冻结但拦不住长算)?建议 safety=generation、liveness=单调墙钟 ms 双轨,拒绝把 frame age 改成逻辑 tick。给出默认值需第一条真实任务的帧率/识别延迟。
- 错误 kind 拆分:把 wait 超时(Tier B 可捕获)与全局/指令预算耗尽(Tier C 不可捕获)拆成不同 kind;确认 StaleObservation retryable 默认与是否绑定层内建小重试(与 Q2 拍板项统一);确认向脚本暴露原生 pcall 还是仅受控 bot:try(fn) 以保证 Tier C 不可吞。
- 回放/可追踪的真实边界:明确 M1 交付的回放是 ADR-006 Exploratory 帧复核(不承诺决策级重演),据此改写所有'重演决策'措辞与 M1 退出契约(line723),把可达性/终止性/能力一致性三项净损失显式写入,不假装无损。
- 第一条真实日常任务 + M0 场景映射 + 首游戏(偏差 A:鸣潮 vs 卡厄斯梦境,真机现停角色详情页):解锁'同帧多查'效率前提、API 表达力验证、以及 Q3 Layer3 / Q6 interrupts / Q7 子任务是否 M0 需要的 YAGNI 判断——头号硬阻塞。
- YAGNI 压力测试:interrupt drain-to-fixpoint、子任务加载图+环检测、Layer3 参数化识别、trace 哈希链——逐项问'第一条任务真的需要吗',按'简洁优先'把不需要的推迟,避免在无真实用例下过度工程。
- 沙箱确定性地板补全:补 setmetatable/getmetatable/raw*/next/tonumber 的逐个判定(尤其 setmetatable→__gc/__index 的逃逸与非确定风险),定 Lua 5.4 integer/float 子类型在变量/边界的归一或拒绝规则,补 __gc/字符串地址 tostring 的消毒。
- Frame 生命周期有界化:定 stale Frame 的强制释放/Frame 数量上限,兑现 §3.6/§19,防止脚本持有多帧导致 8MiB×N 内存堆积。
- 信任模型(Q9 前提):项目脚本仅可信作者还是可能第三方——决定 string.rep/format 是否需显式上限防内存炸弹、消毒严格度,直接影响 S2 范围。
- 常数标定确认:指令预算 N、500ms 取消 SLA 是否含纯 Lua 死循环、pause 是否 M0 必需(并明确若后置则 Q2/Q5 哪些机制同步裁剪)、各 kind retryable 默认——先给保守占位、S9 soak 前校准。
- ADR 结构厘清:确认 ADR-013 即'采用命令式 Lua 任务模型'及其编号,ADR-001 被取代的论证挂靠,interrupts 是否单列 ADR——阻塞 S0 文书落地。

---

## 8. 待 grill 解锁清单

> 汇总所有 `developerInputNeeded` + `critique.grillTargets`，按优先级排。P0 = 硬阻塞（不定则后续无法起步）；P1 = 机制拍板；P2 = 常数标定 / YAGNI。

### 8.1 P0 — 硬阻塞（critique 明确标注的头号靶点）

以下直接取自 `critique.grillTargets`，按 critique 给出的顺序（前列即最先追问）：

- 双 generation 计数器语义:先画清 FrameId(每 capture,高频)与 TargetGeneration(窗口事件,低频)两个计数器,定死注入层 fencing fence 哪个、lease 字段如何映射 ADR-004、resume/过门各 bump 哪个。这是 S5 地基,阻塞其余一切绑定层裁决。
- max_action_frame_age 度量:墙钟 ms(守 §8.3 物理 liveness、能拦纯 Lua 长算后旧帧)还是逻辑 tick(暂停冻结但拦不住长算)?建议 safety=generation、liveness=单调墙钟 ms 双轨,拒绝把 frame age 改成逻辑 tick。给出默认值需第一条真实任务的帧率/识别延迟。
- 错误 kind 拆分:把 wait 超时(Tier B 可捕获)与全局/指令预算耗尽(Tier C 不可捕获)拆成不同 kind;确认 StaleObservation retryable 默认与是否绑定层内建小重试(与 Q2 拍板项统一);确认向脚本暴露原生 pcall 还是仅受控 bot:try(fn) 以保证 Tier C 不可吞。
- 回放/可追踪的真实边界:明确 M1 交付的回放是 ADR-006 Exploratory 帧复核(不承诺决策级重演),据此改写所有'重演决策'措辞与 M1 退出契约(line723),把可达性/终止性/能力一致性三项净损失显式写入,不假装无损。
- 第一条真实日常任务 + M0 场景映射 + 首游戏(偏差 A:鸣潮 vs 卡厄斯梦境,真机现停角色详情页):解锁'同帧多查'效率前提、API 表达力验证、以及 Q3 Layer3 / Q6 interrupts / Q7 子任务是否 M0 需要的 YAGNI 判断——头号硬阻塞。
- YAGNI 压力测试:interrupt drain-to-fixpoint、子任务加载图+环检测、Layer3 参数化识别、trace 哈希链——逐项问'第一条任务真的需要吗',按'简洁优先'把不需要的推迟,避免在无真实用例下过度工程。
- 沙箱确定性地板补全:补 setmetatable/getmetatable/raw*/next/tonumber 的逐个判定(尤其 setmetatable→__gc/__index 的逃逸与非确定风险),定 Lua 5.4 integer/float 子类型在变量/边界的归一或拒绝规则,补 __gc/字符串地址 tostring 的消毒。
- Frame 生命周期有界化:定 stale Frame 的强制释放/Frame 数量上限,兑现 §3.6/§19,防止脚本持有多帧导致 8MiB×N 内存堆积。
- 信任模型(Q9 前提):项目脚本仅可信作者还是可能第三方——决定 string.rep/format 是否需显式上限防内存炸弹、消毒严格度,直接影响 S2 范围。
- 常数标定确认:指令预算 N、500ms 取消 SLA 是否含纯 Lua 死循环、pause 是否 M0 必需(并明确若后置则 Q2/Q5 哪些机制同步裁剪)、各 kind retryable 默认——先给保守占位、S9 soak 前校准。
- ADR 结构厘清:确认 ADR-013 即'采用命令式 Lua 任务模型'及其编号,ADR-001 被取代的论证挂靠,interrupts 是否单列 ADR——阻塞 S0 文书落地。

### 8.2 三个专属开发者输入（只有开发者能给）

- **第一条真实日常任务是什么**：登录/签到/领取/菜单导航/画面决策？有无弹窗？是否跨文件复用子流程？→ 硬阻塞 **S9 定形**，并决定 Q3 Layer3 参数化识别、Q6 interrupts、Q7 子任务是否 M0 需要（YAGNI），以及 Q1"同帧多查"效率前提、各类常数标定。
- **M0 场景在卡厄斯梦境里对应哪个游戏内流程**：§26 "Home→点击→等 Result→点击 Reset→等 Home" 映射到哪个流程？真机现停在**角色详情页**（非 §26 起点）。→ 硬阻塞 **S9 真机腿**、项目包无法建、M2 对象连带待定。
- **分辨率自适应优先级**：现在就做"均匀缩放归一化"档，还是先严格拒、后补模块？→ 决定 S8 自适应模块是否进首交付；第一条任务是否跑在带 DPI 缩放的非 base 分辨率决定其紧迫度。

### 8.3 各题 developerInputNeeded 汇总（P1/P2）

**Q1. observe() 语义:一次调用一帧 vs capture 一帧多识别器查询**

第一条真实日常任务 / §26 M0 场景的真实决策结构:一次决策典型要查几个识别器?这直接验证'同帧多查'的效率前提是否成立,以及 capture→multi-find→click→recapture 是否匹配卡厄思梦境的实际流程(作者已在文档 132–135 行标注真机当前在角色详情页、非 §26 起点,此点必须开发者补)。其余需拍板项:(1)是否把 raw frame:find 暴露给脚本作者,还是只给 bot:wait/exists/match 便利封装、把裸 capture 藏进宿主(人体工学 vs 控制面的取舍);(2)max_action_frame_age 的实际值(§8.3 默认 750ms),需按目标游戏帧率/识别延迟由开发者定;(3)是否接受用两个名词(capture+find)换确定性/trace 红利,还是坚持单名词 observe()+内部缓存(Model C)——这关乎产品实际用法,只有开发者知道。

**Q2. 命令式 Lua 里怎么保住"不对失效观察下手"(租约 = frame generation + 时间)**

1) max_action_frame_age 的单位与默认:DESIGN 定 750ms 墙钟,改逻辑预算后应重定义为'N 个 tick',还是保留'单调时钟毫秒 + generation 双轨'?需拍板逻辑预算的单位与默认值。 2) 租约失效是否允许脚本受控重试(可恢复)还是一律判 run 失败:直接影响日常任务健壮性与 Q4 分野,需第一条真实日常任务的形态才能定(见 grill 文末'第一条真实日常任务')。 3) 是否强制'动作后再观察确认'闭环来兜 TOCTOU Type II:每个动作多一次 capture 的成本值不值,需按 M0 场景(§26 在卡厄思梦境对应哪个游戏内流程,真机现在停在角色详情页)拍。 4) ADR-013 是否即'采用命令式 Lua 任务模型',以及 Q2 租约裁决与 ADR-001 被取代的论证挂哪个 ADR 编号——需开发者确认 ADR 结构。

**Q3. 识别器/模板在哪声明:Lua 里,还是 manifest/项目包**

1) manifest 载体:project.toml 内 [[recognizers]] / 独立 recognizers.toml / 每个 flow 旁置声明——影响多任务共享识别器的组织方式。2) 是否 M0 就要 Layer3 参数化识别,还是先纯静态——取决于 line135 待补的'第一条真实日常任务'是否需要'第 N 格'类动态 ROI。3) 动态索引 bot.templates[expr] 的容忍度:完全禁止(只允许静态点号,最强静态保证但限表达力)还是允许但运行时 fail-closed——需开发者定红线。4) manifest ROI 坐标用 FrameSpace 像素@base_resolution 还是 NormalizedSpace(0-1):后者对分辨率更鲁棒但有子像素精度坑(底料分辨率坑),二选一需拍板。

**Q4. 错误模型:raise(pcall)还是返回 nil/false**

未捕获的 StaleObservation 是否由绑定层内建小重试 N 次(Selenium 式 re-capture+find)后再判 run Failed,还是一律立即失败、重试完全交给脚本——取决于"第一条真实日常任务里 stale 有多频繁"的工效/策略拍板,我无法替你定。是否向脚本暴露原生 pcall(便捷重试)还是只给受控 bot:try(fn) 包装(保证无法捕获 Tier-C)——安全 vs 工效权衡,需你拍。各 kind 的 retryable 默认值(如 ActionRejected 到底可不可重试)依赖真实目标(卡厄思梦境)实测行为,需你给数据或先给保守默认。SOL_EXCEPTIONS_SAFE_PROPAGATION 构建标志与异常配置由构建负责人拍(取决于工具链/是否全程 -fexceptions)。

**Q5. 取消/暂停/超时怎么跨协程工作(yield/resume + 宿主插桩)**

需开发者拍板的:(1)指令预算 N(每钩指令数)与全局 max_runtime/最大 slice 数的标定——多少纯 Lua 指令算『太久』才中断,平衡 500ms 取消响应 vs 开销;此值依赖『第一条真实日常任务』(作者自己也标为未知)。(2)pause 是否 M0 必需还是后置里程碑——§9.2 列了 Pausing/Paused,但 M0(§26 裸 demo)可能只需 cancel+timeout;若 pause 延后,M0 就不必现在做 generation-bump/resume 作废整套机器。(3)取消响应 SLA 确认——成功标准『普通等待 500ms 内响应取消』是否为 M0 硬指标,以及是否同样适用于纯 Lua 死循环中断(这决定指令钩子 N 的量级)。(4)Ctrl-C(CLI)与 Engine.cancel(API)是否共用同一取消路径(很可能是,但需确认 CLI 信号→atomic 的接线)。(5)per-op 分层超时(per-capture/per-recognition/per-retry)在 M0 是否全需还是先做子集——取决于 M0 场景/第一条真实任务的形态。

**Q6. 随机弹窗 interrupts 怎么声明(bot:on + max_hits)**

1) 是否新写 ADR-013『任务级 interrupts』并作废对现有 ADR-013 的引用——草案把它当既有条款引用,但 DESIGN.md v0.4 只到 ADR-012,这条必须开发者拍。2) max_hits 默认值与 on_exhausted 默认策略(fail 还是 stop)——取决于第一条真实日常任务里弹窗的实际频率与性质。3) interrupt 是仅 run 作用域,还是可按 step/阶段启停(有些弹窗只在特定阶段预期)——取决于首条真实任务的流程结构。4) handler 能否调子任务、允许多深——耦合 Q7,需开发者定边界。5) 命中弹窗能否『中止主流程判 run 失败』(致命弹窗)还是只能 handle-and-continue。6) 第一条真实日常任务里具体有哪些弹窗(重连?每日奖励?更新提示?)——文档 §26/待补节自己都标注 M0 场景与首条任务未知,这是验证 API 表达力的前提,只能开发者提供,不替他假设。

**Q7. 子任务/复用:Lua 函数互调,还是宿主级任务注册表**

1) 递归策略拍板:§8.5『v1 禁止递归』对『同 VM 内普通 Lua 函数递归』是否强制——加运行时调用深度守卫(阈值取多少?)还是带论证放宽?这是纯 Lua 函数天然违反、必须开发者定的点。2) 跨文件复用是否 M0/M1 就需要:若第一条真实日常任务单文件足够,bot:load_subtask 可按 YAGNI 推迟到真实多任务共享子流程时再落地——需要开发者告知第一条任务的复杂度与是否跨任务共享子流程(与议程末尾『第一条真实日常任务』同一未知)。3) 子任务边界输出形态:单返回值 vs 多标量输出表,取决于真实任务数据流。4) 子任务命名/寻址是否需要项目内命名空间或版本标记(与 §3.7 schema 版本化平行)。

**Q8. 能力/兼容性门(分辨率/目标)在哪把关 + 要不要现在留自适应口子**

以下必须开发者拍板,不替你假设:(1) 分辨率自适应优先级——文档'需要开发者补充'里已列此条。我的建议是 M0 strict + 留恒等缝、自适应作后置第一模块;但你第一条真实日常任务是否就跑在非 base 分辨率(带 DPI 缩放的笔记本/多显示器)决定这个模块的紧迫度。(2) 目标游戏的实际 scaling_mode(fit/fill/stretch/none)——卡厄思梦境/鸣潮/NIKKE 的渲染缩放模式是实测事实,决定 M0 数据模型是否真的需要 offset/viewport(若都是 none 且你只跑固定分辨率,offset 档可先留空实现)。(3) DPI 处理策略——§6.3 把 DPI Awareness 列为硬需求;你机器是否恒为 per-monitor-v2?门是否要在 M0 就把 DPI 归一(我建议要,否则严格拒会误伤 DPI 放大的同逻辑分辨率目标)。(4) 标准化的 base 分辨率(1920×1080?你的截图/采集设置)。(5) M0 场景/第一条真实任务(文档 open 列表已列)——决定分辨率自适应是否根本不在首个交付的关键路径上;若首任务恒在 base 分辨率,自适应可安心后置。

**Q9. 沙箱:暴露哪些 Lua 标准库,禁 require 后子任务复用怎么办**

信任模型是最需要拍板的前提:项目包脚本只来自可信作者、还是可能来自第三方?这决定消毒严格度(是否需给 string.rep/format 显式上限防内存炸弹,还是交给内存/指令预算兜底)。其次:(1)是否向脚本暴露随机 bot:random——取决于第一条真实日常任务是否需要随机化行为(如反检测抖动),若需要,种子策略(每 run 固定 vs 记录回放)要定;(2)超越函数(math.sin/exp)在 M0 是否就要求跨平台位级复现,还是接受同平台确定即可,这决定 math 库消毒严格度;(3)M0 是否就需要 load_subtask,还是先单文件任务、子任务复用后置——取决于尚未确定的 M0 场景与第一条真实任务的复杂度。

**Q10. 调度：只做"一次一个任务"、无 daemon，还是为托盘常驻留 Engine API 边界**

1) 第一条真实日常任务的**执行频率**：分~时级则进程启动成本(建 state/sandbox/能力协商/Controller 连接)可忽略，no-daemon 无痛；若存在秒级高频轮询需求，需重新评估(但现行 §2.3 已把常驻列为不做)。2) M4 托盘 GUI 的形态定位：是『顺序发火 UI，仍一次一 run』(本裁决前提，零架构债)，还是『真正的并行调度器』——后者会与 §9.1 并发模型和确定性灵魂约束正面冲突，须重新谈判。此为 product 方向，须开发者拍板。3) 将来跨游戏顺序运行时，是否愿意为『每 run 重建 Controller 连接的开销』付代价，还是届时优先做常驻保持连接——属 M4 优化取舍，非当前决定。这三条我不替开发者假设。"

### 8.4 元决策（跨题，建议 grill 优先解决）

- **ADR-013 归属**：DESIGN v0.4 §24 只到 ADR-012，不存在 ADR-013；Q6 草案却把它当既有条款引用。极可能 ADR-013 就是待写的《采用命令式 Lua 任务模型》主 ADR 本身。澄清前不能把 ADR-013 当权威条款引用。
- **ADR-001 被取代的论证**：ADR-001（显式状态机，理由：直观/可验证/易回放）正是被推翻的决策；新 ADR 须论证 Lua 如何重新赢回"可验证/易回放"（回放靠 ADR-006 Exploratory 帧复核 + 全量 trace 重演决策，非重跑游戏必同果），可验证性是主要牺牲项。
- **M1 退出契约重写**：§22 line723 "所有状态和资源引用可在加载时验证" 与命令式 Lua 直接冲突，建议改为"已声明资产加载期校验存在+哈希；可静态获知引用 fail-closed；完整可达性/终止性下沉为运行时预算 + lint"。

---

_本决策包由 workflow `wf_54d09969-ad0` 的 journal.jsonl 组装：ground×5 · grill×10 · synth×3 · critique×1。_
