# 2026-08-29 跨仓库解禁改动评审 — 必须修复项

从一次评审的记录中提取。共 16 条 must（去重后 16 条）、35 条 may、40 条 note；
复核阶段维持 81 条、推翻 3 条。本文件只收录 must。

## 1. [uf-chaos] umbraflow-kit.json:2

**host 被改成本机绝对路径 file:///E:/...，写进了已提交的项目配置**

HEAD 的 host 是 https://github.com/Kaoru-misono/UmbraFlow，工作树改成 file:///E:/github/umbraflow-cpp-annotation-design/install。owner 明确拒绝把机器本地绝对路径写进项目的已提交配置；这个值只在这台机器上成立，换机器、换 worktree 名字即失效。框架侧确实允许 file:// host（release-bundle.cpp:472「host must be an https:// or file:// URL」），但「能接受」不等于「可提交」。

*触发场景*：任何其他机器（CI、另一台开发机）上执行 `project init`：E:/github/... 不存在，init 失败；worktree 目录改名（当前是 -annotation-design 分支专用 worktree）也同样失败。

*建议*：把 host 回退到 HEAD 的 https://github.com/Kaoru-misono/UmbraFlow。本机测试时用框架已提供的运行时选择：`project upgrade [--source PATH] [--release NAME]`（modules/project/source/project/command.cpp:811），或本地未跟踪的覆盖，不要改跟踪文件。若确实需要项目长期指向一个本地安装目录，那是 owner 的决定，不在实施者授权范围。

## 2. [uf-chaos] umbraflow-kit.json:4

**release 从 latest 钉到 2026-08-29-ocr-characters，越权且钉的是不含解禁的 release**

实施者被明确要求不要动 release 钉定，diff 仍改了。而且这个 release 并不包含本次解禁：install/releases/download/2026-08-29-ocr-characters/project.exe 时间戳 Aug 29 00:48，早于框架提交 822be16a（04:06）和尚未提交的 handler 组合改动；框架侧对应的 release note 是未跟踪的 docs/release-notes/2026-08-29-project-handler-tool-calls.md，install 目录下没有以它命名的 release。docs/status.md CH-09 自己也写「其余六个 Tool 的声明等待新 release 并更新 umbraflow-kit.json」，所以此刻钉这个名字既没授权也没用处。

*触发场景*：owner 按 README 执行 `project init`：拿到的是只带 OCR 字符改动的旧 release，六个组合 Tool 一旦声明就会被旧 release 的叶子契约拒绝；下次真正的 release 出来又要再改一次钉定。

*建议*：回退到 HEAD 的 "latest"。本次验证用哪个本地 release 由命令行 `project upgrade --release NAME` 指定即可；是否以及何时钉定由 owner 决定。

## 3. [uf-chaos] .vscode/tasks.json:9

**未跟踪的 .vscode/tasks.json 指向已不存在的 Python 模块，是遗留垃圾**

任务运行 `python -m tools.hand_recognition.task run --image work/hand-check/input.png --pool work/hand-check/pool.json`。但 tools/hand_recognition/ 下只剩 README.md 和 __pycache__（ls 证实无 task.py），而 tools/hand_recognition/README.md 自己写「没有 Python 识别流水线」。这是被删掉的 Python 识别流水线的编辑器任务残留。文件本身不含本机绝对路径（用的是 ${workspaceFolder}），但内容已死。

*触发场景*：在 VS Code 里运行该任务：ModuleNotFoundError: No module named 'tools.hand_recognition.task'。

*建议*：不要提交这个文件；直接删除（它是本 session 产生的未跟踪文件）。如需保留编辑器任务，指向 tasks/recognize-hand.luau 的实际运行方式。

## 4. [uf-chaos] runtime/hand-recognition/candidates.example.json:1

**candidates.example.json 无任何引用，是不该入库的示例文件**

这个 160 字节文件写的是 `chaos.hand-candidates/v2` 请求样例（roster + shared）。全仓 grep（plugin/tasks/schemas/tests/scripts/docs/README/tools）没有任何地方引用 `candidates.example`；umbraflow-project.json 的 resources 也未声明它。真正构造这个请求的是 plugin/battle/catalog.luau:13，从 battle_plan 的 roster 直接生成，不读文件。

*触发场景*：提交后下一位读者会以为运行时读它、或以为 roster 应在这里改，而实际改这里没有任何效果。

*建议*：删除；若想留下请求形状的说明，把它写进 schemas/ 里的 schema 或 catalog.luau 的注释。

## 5. [uf-chaos] content/user-temp-file.md:107

**健康时的排序次序与代码不一致，且漏掉了「优先攻击」和「未识别/垃圾牌垫底」两个判据**

文档写「健康时沿用角色特殊卡/中立/基本、避免连续同角色、核心角色及角色内部顺序」，读者会理解为先按类别（特殊>中立>基本）再避免连续同角色。代码的实际比较键顺序是：①有牌库定义且非 junk（未识别=2、junk=1、正常=0）→ ②避免连续同角色 → ③类别 → ④核心角色 → ⑤角色内部 named_order → ⑥是否 attacks → ⑦费用 → ⑧槽位。即「避免连续同角色」排在类别之前，并且健康时还有一个文档完全没提的「优先攻击牌」判据（owner 第 25 行的策略要求正是这一条）。低血量时顺序也不同：①定义 → ②治疗/护盾 → ③同角色 → ④类别 → ⑤核心 → ⑥内部顺序。

*触发场景*：手牌里同时有海德玛丽的 1 费特殊卡（上一张刚打的是海德玛丽）和米卡的 1 费基本卡：按文档描述应先打海德玛丽特殊卡（类别优先），按代码会先打米卡基本卡（sameOwner=0 排在 category 之前）。owner 按文档复核策略时会得出与实际相反的结论。

*建议*：把这一段改写为逐级比较键的实际顺序，明确写出健康与低血量两套键序，包括「没有牌库定义的牌排最后、junk 次之」和健康时「攻击牌优先于非攻击牌（在角色内部顺序之后）」。

## 6. [framework] modules/operator/source/operator/tool-executor.cpp:251

**重放分歧被落成帧的 terminal_failure，嵌套一层后可被父 handler 吞掉并以 Confirmed 结束**

决策第 4 条说「Replay divergence is a hard refusal」。但实际路径是：子坐标分歧（persistToolCallPosition 里 divergedToolCallField → terminateToolRun + ActionRejected）或 validateToolCallChildren 失败，只是让 runHandler 返回一个 Result 错误；ToolRuntimeExecutor::invoke 随后把这个 provider 错误按 RecordedChildren 分类为 providerFailureCompletion，并调用 completeToolCallDispatch 把该帧写成 terminal_failure。completeToolCallDispatch（ledger.cpp:11102 起）不检查 requireLiveToolRun，所以 run 已 terminated 仍能写终态。深度 1 时表现为「返回 TerminalFailure 信封」（test-tool-dispatch.cpp:513 甚至把它当预期）；深度 ≥2 时，子 Project 帧 C 的 TerminalFailure 通过 project-tool-dispatch.cpp:123 的 toolCallAnswer 变成普通 ok=false 信封交给父 handler P，P 的 Luau 代码 pcall/判断后照常 return，P 的 validateToolCallChildren 计数一致 → P 被写成 Confirmed。分歧就这样被一次成功掩盖，与「hard refusal」和「a caught refusal may not hide it」相悖。

*触发场景*：P（mutating 或 read-only 的 composed Project Tool）崩溃后重入；P 的第 2 个子调用是 Project Tool C，C 也处于 dispatching；C 重入时其记录的第 1 个子调用是 framework.screen.capture，而这次 C 的 handler 走了另一分支调用 framework.workflow.now → C 的 persistToolCallPosition 终止 run 并返回 ActionRejected → C 的 handler 失败 → C 的 executor 把 C 写成 terminal_failure（run 已 terminated 但无人检查）→ P 收到 {ok=false,error={code=ActionRejected,...}} → P 的源码 `local r = C(...); if not r.ok then return {fallback=true} end` → P 完成为 Confirmed，caller 得到 ok=true，而 tool_root_requests.state='terminated'。

*建议*：把「分歧」从可捕获的终态里拿出来：(1) 在 ToolRuntimeExecutor::invoke 里，provider 返回后除了 hasUnresolvedToolDescendants，再查一次 run 是否仍 running（或让 completeToolCallDispatch 对 terminated run 拒绝写终态），run 已被 terminateToolRun 标记时直接把 Result 错误向上返回、不写 completion；(2) 这样每一层 invokeChild 都拿到 Result 错误而不是信封，p_children->failure 粘住后父 handler 无法用 return 掩盖；(3) 相应修正 test-tool-dispatch.cpp:513 的预期（分歧 → dispatch 返回错误而非 TerminalFailure 信封），并补一个深度 2 的「父 pcall 子分歧后 return」用例，断言父帧不得 Confirmed。

## 7. [uf-chaos] umbraflow-kit.json:2

**kit 配置写入了机器本地绝对路径 file:///E:/... 作为 release host**

`host` 从 GitHub 发布地址改成了 `file:///E:/github/umbraflow-cpp-annotation-design/install`，`release` 改为 `2026-08-29-ocr-characters`。这是一台机器上的 worktree 安装目录，进入项目配置后其他机器、CI 和未来的自己都无法同步 bundle；owner 明确拒绝把机器本地绝对路径写进项目配置。

*触发场景*：任何非本机 checkout（或本机把 worktree 删掉/改名后）执行项目升级/同步命令时，host 不可达，整个项目无法拉到 release。

*建议*：把 host 恢复为发布地址并把 release 钉到一个已发布的版本名；本地调试用的 file:// 覆盖放在被 .gitignore 的 work/ 或环境变量里，不进 diff。若这次改动必须依赖尚未发布的框架 release，在提交说明里写明阻塞而不是把本机路径入库。

## 8. [uf-chaos] scripts/check_docs.py:24

**为保留一条指向已删文件的链接，给文档门加了走 git 历史的兼容分支**

`docs/decisions/2026-08-27-battle-loop-observes-before-each-input.md:10` 和 `:54` 仍链接 `plugin/strategy/battle-loop.luau`（已删除）。为了不改这份文档，`check_docs.py` 新增 `historical_pointer_exists`：只要文档在 `docs/decisions/` 下、工作树与 HEAD 一致、且 `git log -1` 的那个 commit 里目标存在，就把断链视为合法；`tests/documentation/test_links.py` 又专门为这个分支写了测试。这正是 CLAUDE.md 禁止的「为了让现有文件不动而加的兼容路径」：读者看到链接会点开一个不存在的文件，门却说 OK；而且该分支依赖三个子进程调用和「文档必须与 HEAD 逐字一致」这一隐含条件——任何人改动该决策文档一行（哪怕修错字），断链立刻重新变成错误，且错误原因与所改内容无关。

*触发场景*：（1）在浏览器/编辑器中点击 docs/decisions/2026-08-27-*.md 第 10 行链接 → 404；（2）有人修正该文档一个错字并运行 `python scripts/check_docs.py` → 报 broken link，与所改内容无关；（3）在没有 .git 的导出目录中运行门 → `git` 调用失败被当成 False，同样报错。

*建议*：删除 `historical_pointer_exists` 与 `tests/documentation/test_links.py`。把决策文档里两处链接改成钉住版本的 URL 或纯文本（例如 `plugin/strategy/battle-loop.luau`（已于 2026-08-29 删除，见 `docs/decisions/2026-08-29-battle-project-tools.md`）），这符合 owner「指向发布位置并钉版本」的偏好，也让新决策文档的 supersede 关系在旧文档里可见。

## 9. [uf-chaos] docs/design/battle-as-project-tools.md:10

**「同一套 Tool 面服务三种控制者」在当前工作树不成立，驱动绕过 Tool 面直接调本地模块，且这一临时状态未写入契约**

契约第 10 行声称七个 Tool 同时服务 Luau 驱动、agent、人。实际只有 choose_action（及两个事件 Tool）注册；六个战斗动作只是 `plugin/battle/entry.luau` 导出的普通模块函数，`tasks/battle.luau` 通过 `require("battle/entry")` 把它们当本地函数传给驱动，agent/人今天无法通过任何 Tool 名调用 observe_battle/read_hand/inspect_card/play_card/end_turn/resolve_overlay；它们也不作为 Project Tool 进入准入与账本（其内部的 framework.* 子调用被记为交互 chunk 自己的调用）。契约正文只在第 41 行说「其余六个要等框架解禁」，没有说明解禁前驱动以模块函数方式调用、Tool 面对 agent 缺席这一过渡形态；status.md CH-09 有提，但契约文档本身与代码不一致。

*触发场景*：一个在线 agent 读取 catalog 后想调用 chaos.dream.read_hand：该名字不存在，只能自己重建 observe+逐矩形读+匹配；而 Luau 驱动走的是另一条路径（本地 require）。同一动作两种调用形态，正是文档说不存在的「两条并行流水线」。

*建议*：二选一并写进契约：(1) 在契约「一句话」/约束 3 处明确当前形态——解禁 release 发布并 pin 之前，六个动作以 tool_closure 模块函数形式仅供驱动调用，不构成 Tool 面，agent 不可达；(2) 或直接等待 release、在同一次改动里注册七个 Tool 后再合入，让文档与代码同时为真。

## 10. [uf-chaos] docs/design/battle-as-project-tools.md:36

**契约约束 1「每个 Tool 只读一帧」与 inspect_card（源帧+持帧）、play_card（拆成两次调用）的实现不一致，契约未同步**

代码里 inspect_card 读源帧定位、再从 input.hold 返回的 held 帧读面板（两帧）；play_card 拆为两次独立调用（第一次发槽位键并把待确认槽位存入 session，第二次凭新帧的 card_selected 覆盖层发 ENTER），表格第 30 行「槽位键 → 提示板凭据 → ENTER」读起来像一次调用，第 36 行约束仍写「只读一帧」。例外只记录在 docs/decisions/2026-08-29-battle-project-tools.md:13-16，契约（被明确当作对照标准的文件）本身没改。

*触发场景*：agent 读契约后一次 play_card 调用期望 `{selected=true, receipt}`，实际拿到 `selected=false`，需要再采一帧再调一次；或对 inspect_card 传入 held 帧当 screenshot，被 `inspection requires an unobscured battle` 拒绝。

*建议*：改契约：约束 1 改为「Tool 不等待、不轮询；每个 screenshot 依赖以显式凭据进出。inspect_card 是唯一读两帧的 Tool（源帧定位、hold 返回的 held 帧读面板）」；play_card 行写明两段协议：`selected=false` 表示已发槽位键并登记 session 待确认，须以新帧再调一次，观察到 card_selected 且 `selection.use` 在 offered 中才发 ENTER 并返回 `selected=true`。处理本身合理：两段拆分保住了「ENTER 用新观察证据」，驱动只多一次 settle，而不是把等待塞进 Tool。

## 11. [framework] README.md:33

**README 仍写着「Project Tool handler is a leaf」的旧契约**

README 顶层架构段落仍声明 handler 是叶子并会被拒绝发起 Tool 调用，与本次代码（ledger.cpp 已删除「Tool admission refuses a nested call because Project Tool handlers are leaves」分支；ScopedToolRun::callTool 现在转发到 issuing door）以及 2026-08-29 决策文档直接矛盾。CONTEXT.md、pitfall、plan 都改了，README 漏了。

*触发场景*：新读者或下游 uf-chaos 作者读 README 第 33 行，得出 handler 不能组合 Tool 的结论，与 PUBLIC-CONTRACT.md 第 196-222 行相反。

*建议*：把这一句改为与 CONTEXT.md「Tool call」词条一致的说法：handler 可通过同一准入门槛发起被记录的子调用；或者直接删掉该句，仅保留「adapter 构造不出可执行的东西」的论点。

## 12. [uf-chaos] plugin/battle/observe.luau:8

**读数按 `reading.reader` 匹配，框架实际字段是 `id`，所有 readouts 永远为 null**

`M.text` 只在 `reading.reader == id` 时返回文本，但框架 `screen.observe` 返回的 `state_resolution.readings[]` 每条记录的键是 `id`（`modules/task/runtime/resolution.luau` 的 `reportedReading` 返回 `{id, kind, lines, reason}`，`task-host.cpp` 把 `cycle:resolve_readings(state)` 原样放进 `readings`）。项目里没有任何地方产生 `reader` 键；被删除的旧 `battle-loop.luau:87` 用的正是 `row.id == id`。三处测试 fixture（tests/strategy/battle-loop.luau:121、tests/recognition/hand-task.luau:16/70、scripts/test_battle_framework.py:82）都伪造了 `reader`，把错误一起固定下来了。

*触发场景*：真机运行 `tasks/battle.luau`：`observe_battle` 的 `readouts.hand_count/energy/hp/max_hp/ep` 全部 null → `read_hand` 因 `type(count) ~= "number"` 直接返回 `state = "unresolved"` → `choose_action` 永远答 `wait`(`hand_unresolved`)，驱动空转到 `cycle_cap`（240 轮 × ≥3 s）。灵光一闪覆盖层的 `title/body/cost` 也全 null → 全部 `unreadable_choice` → `stop`。`play_card` 的 `assert(type(frame.readouts.hand_count) == "number")` 必炸。

*建议*：把 `reading.reader == id` 改为 `reading.id == id`，并把三处 fixture 的 `reader =` 改成 `id =`（fixture 应照抄框架真实形状，而不是照抄项目代码）。

## 13. [uf-chaos] plugin/battle/entry.luau:56

**`leave` 表面的动作 id 写成 `continue`，模型声明的是 `leave`，resolve_overlay 在离开页必然报错**

`resolve_overlay` 对 `leave` 表面要求 `offered.binding == "leave.continue" and offered.action == "continue"`，但 RuntimeModel 里该 Binding 的动作 id 是 `leave`。其余六个 binding/action 对（`inspiration.choice-N/choose`、`reveal.close/close`、`confirm.accept|dismiss`、`reward.claim/claim`、`settle.claim/claim`、`selection.use/use`、`battle.end-turn/end-turn`）我逐条核对过与模型一致，只有这一个错。

*触发场景*：agent 或人对 `leave` 页调用 `chaos.dream.resolve_overlay{screenshot}`：`frame.surface == "leave"` 分支设 `action = "continue"`，循环里找不到匹配项，落到 `error("overlay action not offered; no coordinate fallback")`。（当前驱动在 `leave` 上 `choose_action` 会先 `stop`，所以只在 Tool 被直接调用时触发。）

*建议*：改为 `binding, action = "leave.continue", "leave"`，或者把模型里的动作 id 改成 `continue`，二选一并对齐 `docs/run-loop.md`。

## 14. [uf-chaos] tasks/recognize-hand.luau:4

**`project.read_text` 的结果字段是 `content`，脚本读的是 `.text`，任务首行即崩**

框架 `framework.project.read_text` 的输出成员是 `content`/`content_hash`/`path`（tool-invocation.cpp 的 `projectReadTextOutputMaterial`），没有 `text`。`project.read_text{path=...}.text` 为 nil，`json.parse(nil)` 抛错，标定任务永远跑不起来。

*触发场景*：运行 `tasks/recognize-hand.luau`（即 scripts/test_hand_task.py 之外的真机标定）：第 4 行 `json.parse(nil)` 报错，任务终止。

*建议*：改为 `.content`。

## 15. [uf-chaos] plugin/battle-tools.luau:1

**保留了两个注册根（main / battle-tools）和「等框架解禁再切」的开关，属于兼容层**

`battle-tools.luau` 是一个「解禁后把 `tool_closure.entry` 从 `main` 改成 `battle-tools`」的备用根；`plugin/main.luau:1` 注释「Battle mutators remain callable project modules until the framework release supports child calls」；`umbraflow-project.json` 的 `$comment` 写「When the six child-calling battle Tools are declared, set entry to battle-tools」。框架工作树里 `docs/decisions/2026-08-29-project-handlers-compose-recorded-tool-calls.md` 已经落地解禁，本项目又已把 `umbraflow-kit.json` 指向该工作树的 install。这就是设计文档自己禁止的「开关/过渡代码」（battle-as-project-tools.md「不做什么：不保留兼容层…不留开关、不留回退分支」），而且两条根并存意味着 `main` 导出的 `choose_action` 与 `battle-tools` 里的六个 Tool 有两个入口清单要维护。

*触发场景*：任何人读 `umbraflow-project.json` 都要猜哪一个 entry 是真的；六个 Tool 的实现已在生产路径里但没有声明，agent 通过 catalog 看不到它们，只能靠 Luau 驱动直接 require 项目模块调用——这正是决策文档要消除的形状。

*建议*：只留一个根：把六个 Tool 的声明与 `tool_bindings` 直接写进 `umbraflow-project.json`，`main.luau` 直接导出 `battle.*`，删除 `battle-tools.luau` 与两处「等解禁」注释。若当前 kit 指向的 install 尚不能注册子调用 Tool，那就先不合并这批文件，而不是留开关。

## 16. [uf-chaos] umbraflow-kit.json:2

**提交配置里写入了机器本地绝对路径 `file:///E:/github/...`**

`host` 从发布地址改成了本机 worktree 的 install 目录，`release` 改成 `2026-08-29-ocr-characters`。这是只在这台机器上成立的路径，owner 明确拒绝把机器本地绝对路径写进项目配置；同时它把项目钉在一个未发布的 worktree 上。

*触发场景*：其他机器或 CI 同步 kit 时 `file:///E:/...` 不存在，`upgrade` 直接失败。

*建议*：改回发布 host 并 pin 一个已 cut 的 release 名；本地覆盖用未跟踪的本地文件或命令行参数，不进 git。
