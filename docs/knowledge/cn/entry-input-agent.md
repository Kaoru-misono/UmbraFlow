# entry/input-agent：标注前端

`entry/input-agent` 就是 `umbra-input-agent` 这个程序。它为一次标注会话服务一条命令队列，
对象是一个裸窗口：读一行、投递它点名的输入、把做决定所依据的帧截下来、在 results 文件上作答。
它是第三个 `trace::FrontEnd`——既不是可信 Luau 框架跑的 task，也不是驱动已加载项目的 operator
——并且在它写的每一行上都这么说。

它于 2026-07-31 离开 `entry/m0-demo`。在那之前它是冻结的 M0 demo 的 `input-agent` 子命令，
而目录名是它身上唯一还算 demo 的东西：一天之内它长出了持久化的 queue cursor、
被抽出来并有变异证明的服务循环、annotation 层之下的 drive 层，以及前端盖章。
旧拼写 `m0-demo input-agent` 现在只打印程序去了哪里，然后以失败退出。

这个目录也拥有两个程序仍然共用的东西——帧 PNG 写出、路径围栏规则、目标选择与捕获会话搭建、
JSON 字符串转义、错误文本，以及命令行解析原语。方向是刻意的：`m0_demo_support` 链接
`input_agent_support`，反过来绝不发生，这样将来退役冻结 demo 是一次删除，而不是又一次抽取。

## 它为什么存在

标注会话要量的是一个裸窗口。它没有项目、没有 runtime manifest、没有 `ResolvedPage`、
也没有已标注元素——量屏幕这件事本身**就是**产出标注的过程。所以 agent 除了目标与观察之外
不授权任何东西：它就是产品用的那条投递路径，只是一次暴露一条命令。

会话的非提权一侧——`umbra-authoring`、一段脚本或者一个人——向 queue 文件追加 JSON 行，
再读 results 文件。agent 只启动一次，目标完整性需要时提权启动，然后长驻。
这样 UAC 边界就集中在一个长期存活的进程上，而不是每次点击弹一次。

```text
umbra-input-agent --hwnd 0xHWND --queue q.jsonl --results r.jsonl --output-dir DIR
```

下面的围栏检查带来两条操作规则：queue 与 results 必须是两个文件，且都要在 `--output-dir`
**之外**；agent 是长驻进程，所以要 detached 启动。

## 分层

三个文件、三种关注点，每一层都能脱离下面两层单测。

- `loop.{hpp,cpp}` —— `runInputAgentQueueLoop`，服务循环，也是唯一决定「一次 run 如何结束」
  的地方，架在 `IInputAgentSession` 端口之上。`InputAgentResultWriter` 也在这里，
  前端盖章就盖在它上面。
- `annotation.{hpp,cpp}` —— `AnnotationSession`，annotation 层：输出路径围栏、before/after
  取景、PNG 编码、results 行的形状。将来标注会话长出的动词——读一块区域、由读出来的东西
  提议一个元素——归在这里。
- `drive.{hpp,cpp}` —— `IInputAgentDrive` 与 `WindowInputAgentDrive`：把一次输入按一次观察
  投给窗口，再拿回一帧。它不认识文件、不编码图像、不解析命令，所以标注会话新造的东西
  一样都到不了它这里。

`IInputAgentDrive` 刻意不是 `engine::IActionSink`，理由与 `IInputAgentSession` 不是它相同：
那个端口说的是 engine 的词汇——针对**已标注元素**的一次已授权动作——而且住在这个可执行文件
不链接的模块里。

`agent.{hpp,cpp}` 是组合根——`runInputAgent` 加上喂它的 `InputAgentQueueReader`——
`main.cpp` 则是它上面的进程边界。

**每一条 results 行都盖着产出它的前端**：行首是 `"front_end":"annotation"`，值来自
`trace::FrontEnd::Annotation`，拼写来自 `trace::frontEndWireName`。`input_agent_support`
为这一个类型链接了 `modules/trace`。agent 不写 `umbraflow-trace/v2` 的行——该 schema 每一行
都带 `runId` 与 `generationId`，而够不到项目的标注会话两者皆无；results 文件就是它的全部
证据流，盖章由 writer 而不是各个 serializer 完成，理由与「`trace::TraceRecorder` 而不是
各个发射方拥有那枚章」是同一条。

## 协议

`entry/input-agent/protocol.hpp` 定义五个 variant：

- `InputAgentCaptureCommand{output}`；
- `InputAgentClickCommand{x, y, outputBefore, outputAfter, settle}`；
- `InputAgentKeyCommand{key, outputBefore, outputAfter, settle}`；
- `InputAgentScrollCommand{x, y, delta, outputBefore, outputAfter, settle}`；
- `InputAgentQuitCommand`。

对应的 JSON object 只接受 `op=capture|click|key|scroll|quit` 及各自的精确字段集；
取景字段在线上的拼写是 `out`、`out_before`、`out_after`、`settle_ms`。
`parseInputAgentCommand()` 拒绝超过 64 KiB、非法 UTF-8、重复/未知字段、错误 JSON number、
空路径、NUL、非有限坐标和超过 5000 ms 的 settle。动作默认 settle 为 400 ms。

`delta` 的单位是整数格（notch），不是 `WHEEL_DELTA` 原始单位；解析时经 `WheelDelta::create`
解析，所以 0、小数、以及原始值放不进 `wParam` 有符号 16 位字的格数，都会在命令入队前被拒绝，
与 `key` 经 `KeyInput::fromName` 解析是同一种做法。

`InputAgentQueueReader` 按 offset 增量读取 append-only queue，接受 LF/CRLF，保留未完成行；
queue 被截断或 pending command 超过 1 MiB 时 fail closed。`InputAgentResultWriter` 每写一条
JSONL result 就 `flushDurably()`。

## queue cursor

`cursor.{hpp,cpp}` 把队列已消费到哪里记在旁边的 `<queue>.cursor` 文件里，
这样重启的 agent 是接着走，而不是把一个活着的目标重新走一遍队列里已有的全部命令。
cursor 路径由规范化后的 queue 路径推出，且不得 alias 两个 IPC 文件中的任何一个——
硬链接本来可以安排出这种事。

`--queue-start refuse|beginning|end` **只**在还没有 cursor、而队列已经非空时才被读取。
`Refuse` 是零值，所以没写明的那个策略是「先问」而不是「重放」。有 cursor 时永远是 cursor 说了算。

循环在写完 results 行**之后**才推进 cursor。在这个空档被硬杀，代价是重放一条命令；
反过来的顺序则会静默跳过一次已经没人能观察到其投递结果的动作。

## 路径围栏

agent 的文件权限面同样是协议的一部分：queue 与 results 必须不同、且都在 output directory
之外；截图路径必须被 confinement 检查限制在 output directory 内，不得 alias IPC 文件。
`platform::FileWriter::createExclusive()` 通过已经验证并保持打开的目录 handle 链做相对
`NtCreateFile(FILE_CREATE)`，拒绝 overwrite、reparse escape、alternate data stream
和目录重命名竞态。

## observe -> act 热路径

`executeClick()` 走的是下面这条，`executeScroll()` 走同一条，只是把 `click` 换成 `scroll`：

```text
reserve fresh before/after outputs
-> capture immutable before Frame
-> ObservationLease::forFrame
-> validateInputAgentPointerAction
-> ResolvedTarget::revalidate
-> requireUnchangedTarget
-> WgcCaptureSession::validateTargetInstance
-> click
-> encode/write before PNG
-> settle
-> capture/encode/write after PNG
```

before PNG 的编码和持久化刷新放在点击之后。真机曾发现，把 1600×900 BGRA 编码、写盘、
`FlushFileBuffers` 放在 capture 与 click 之间，会消耗 750 ms lease budget 并制造非预期的过期。
移动后仍保存同一个 immutable pre-click `Frame`，但不再让取证 I/O 延长 observe -> act。

按键不设 lease 新鲜度闸：它不点名位置，旧观察没法让它落错地方；唯一还要紧的是目标被替换，
而这由 generation 与 `requireUnchangedTarget` 覆盖。

命令解析错误会写一条失败 result 后继续；target revalidation/instance 失败则写 result
并停止 agent。每条命令完成后清空 audit，防止长驻代理的记录无限增长。

## 用哪个构建

agent 必须是 **release** 构建。debug 下识别约 1030 ms，而 lease 只有 750 ms，
于是每个动作都以 `StaleObservation` 失败——见 `docs/pitfalls/capture-and-target-selection.md`。
`umbra-authoring` 用 debug 没问题：它只读已经截好的帧。

## 测试

`tests/CMakeLists.txt` 把 `tests/input-agent/` 组成 `test-input-agent`，
链接 `${PROJECT_NAME}_input_agent_support`。

- `test-agent.cpp` 固定严格 JSON command grammar、UTF-8、settle 上限、路径围栏、增量行分帧、
  queue 截断/大小上限、handle 相对的独占输出、每条命令清 audit、client 边界、
  过期 generation 拒绝，以及 cursor 的续跑/拒绝行为。
- `test-loop.cpp` 固定一次 run 结束的各种方式——停机命令、无法解析的行、idle timeout、
  一次作答是否重置倒计时——以及每一条答复都带前端盖章，包括循环自己写的那两条。
- `test-annotation.cpp` 固定 drive/annotation 拆分买到的那道接缝：越界的输出在 drive
  被要求观察之前就被拒；before 帧只在投递之后才编码，好让 observe->act 窗口里没有慢活；
  窗口被替换是唯一会结束整次 run 的失败；`delivered` 跟着 drive 的答复走，
  而不是会话自己设的一个标志。
- `test-args.cpp` 固定必需的 file IPC 路径、默认值、无 cursor 时的队列策略与拒绝边界。
- `test-target-setup.cpp` 固定目标重校验与空 client area 的拒绝。
- `test-error-text.cpp` 固定失败离开进程时那一行里的 error kind、context 帧与 native origin。

这些是行为与边界测试。它们覆盖不到的部分——投递是否真的到达高完整性目标、before/after
图像变化、真机延迟下的 lease——来自真机会话。

## 与产品代码的关系

入边：一个已存在的 append-only queue、一个 results 路径、一个受围栏的 output directory。

朝 `controller` 的出边：discovery（`enumerateCandidates`、`resolveTarget`、
`ResolvedTarget::revalidate`）、DPI（`ensurePerMonitorAwareV2`）、capture（`WgcCaptureSession`、
`Frame`）、input（`DeliveryTarget`、`ObservationLease`、`click`、`scroll`、`keyPress`、
`releaseHeld`、`AuditLog`）。朝 `image`：把捕获帧编码成 PNG。朝 `trace`：前端值与它的
wire name，仅此而已。

`modules/annotation` 与 `modules/engine` 和这个程序没有链接边，理由不是分层洁癖而是领域：
标注会话手上没有已标注的东西可供授权。`entry/cli` 的 `drive` 子命令刻意在 runner adapter
一侧重新实现这套协议语义——行大小上限、fresh results 文件、输出围栏——而不是链接到这里。

## 相关

- [`entry-m0-demo.md`](entry-m0-demo.md) —— 这个程序长出来的那个冻结 demo，两者至今共用底座。
- [`entry-cli.md`](entry-cli.md) —— 产品 runner，以及它的 `drive` 协议欠这条协议的东西。
- `docs/pitfalls/capture-and-target-selection.md` —— 为什么输入走这个程序而不是手搓
  `PostMessageW`。
