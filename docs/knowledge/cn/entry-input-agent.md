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

一条服务循环架在两个端口上，每一层关注点都能脱离它下面的那些单测。

- `loop.{hpp,cpp}` —— `runInputAgentQueueLoop`，服务循环，也是唯一决定「一次 run 如何结束」
  的地方，架在 `IInputAgentSession` 端口之上。`InputAgentResultWriter` 也在这里，
  前端盖章就盖在它上面。
- `annotation.{hpp,cpp}` —— `AnnotationSession`，annotation 层：输出路径围栏、before/after
  取景、PNG 编码、矩形围栏、results 行的形状。标注会话长出的动词归在这里，`read` 是第一个。
- `drive.{hpp,cpp}` —— `IInputAgentDrive` 与 `WindowInputAgentDrive`：把一次输入按一次观察
  投给窗口，再拿回一帧。它不认识文件、不编码图像、不解析命令，所以标注会话新造的东西
  一样都到不了它这里。
- `text-reader.{hpp,cpp}` —— `IInputAgentTextReader`，annotation 层踩着的第二个端口：
  进去一个观察上的矩形，出来若干 `ocr::TextLine`。它是 drive 的兄弟而不是 drive 上的一个动词，
  因为 drive 只认识窗口、输入和帧，而那一块矩形**说了什么**是作者在量的东西。
- `ocr-text-reader.{hpp,cpp}` —— `OcrTextReader`，活的那一半：ONNX Runtime 上的 PP-OCRv6_small。

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

`entry/input-agent/protocol.hpp` 定义六个 variant：

- `InputAgentCaptureCommand{output}`；
- `InputAgentClickCommand{x, y, outputBefore, outputAfter, settle}`；
- `InputAgentKeyCommand{key, outputBefore, outputAfter, settle}`；
- `InputAgentScrollCommand{x, y, delta, outputBefore, outputAfter, settle}`；
- `InputAgentReadCommand{rect}`；
- `InputAgentQuitCommand`。

对应的 JSON object 只接受 `op=capture|click|key|scroll|read|quit` 及各自的精确字段集；
取景字段在线上的拼写是 `out`、`out_before`、`out_after`、`settle_ms`，矩形字段是
`rect_x`、`rect_y`、`rect_width`、`rect_height`。
`parseInputAgentCommand()` 拒绝超过 64 KiB、非法 UTF-8、重复/未知字段、错误 JSON number、
空路径、NUL、非有限坐标和超过 5000 ms 的 settle。动作默认 settle 为 400 ms。

`delta` 的单位是整数格（notch），不是 `WHEEL_DELTA` 原始单位；解析时经 `WheelDelta::create`
解析，所以 0、小数、以及原始值放不进 `wParam` 有符号 16 位字的格数，都会在命令入队前被拒绝，
与 `key` 经 `KeyInput::fromName`、矩形经 `PixelRect::create` 是同一种做法。

## `read` 动词

```text
{"op":"read","rect_x":262,"rect_y":14,"rect_width":138,"rect_height":36}
{"front_end":"annotation","op":"read","ok":true,"frame_id":7,"reader_ready":true,
 "lines":[{"text":"621/922","confidence_bp":9871,
           "bounds":{"x":262,"y":14,"width":138,"height":36}}],"error":null}
```

（results 是一整行，这里只是为了排版才折行。）

矩形用的是**捕获帧自己的像素空间**——作者在 capture PNG 上量出来的那套坐标——
而不是 `click` 与 `scroll` 用的 client 空间。`capture` 会报 `frame_size`、`client_size`
和两者的 `delta`，那就是告诉作者这个目标上两者是否有差的地方。

这个动词不写任何输出路径，这一点正是它的意义：标注会话里量一段文字，
在此之前只能截一张 PNG 然后用人眼读。

`rect_*` 是整数非负像素，所以小数和负数在解析期就被拒，而不是被截断到一张没有小数的像素网格上。
`PixelRect::create` 拒绝空矩形和溢出矩形，`PixelRect::ensureWithinExtent` 拒绝装不进这次观察的矩形——
第二道闸属于 annotation 层而不是引擎，这样一个本来就读不了的矩形不会成为把 20 MB 模型拉起来的理由。

**一行，不是一块。** 矩形由调用方断言只装一行文字，这样可以完全跳过检测，
既是便宜的那条路，也不怕检测器把一个标签切成两半。真装了好几行的矩形会读成一串胡话而不是失败，
那是 `modules/ocr` 写明的行为，这一层无从发现。`TextLayout::Block` 需要检测模型，
适配器还没跑它，所以没有任何字段用来选 layout——检测落地那天就是这个动词长出这个字段的那天。

**给的是若干行而不是一个拼好的字符串**，每行还带模型自己的置信度（basis point）：
作者量一块区域，需要同时知道读出了什么、以及有多少可以信，拼接会把后者扔掉。
`bounds` 也一起给，因为将来的 block 读法会返回调用方没有提供的框。

### 三种答案，不许混

| 情况 | `ok` | `reader_ready` | `lines` |
|---|---|---|---|
| 模型加载不起来 | `false` | `false` | `null` |
| 这个矩形或这一帧被拒 | `false` | `true` | `null` |
| 区域读过了，里面没有字 | `true` | `true` | `[]` |

`reader_ready` 是这一行上唯一关于整次 run 而不是关于这条命令的事实：false 表示在修好 payload
之前不会有任何一次读能成功，true 表示该去看矩形。空的 `lines` 数组是一个**答案**而不是失败——
一块没有字的区域是关于屏幕的普通事实，把它折进失败那一侧，
会让「弹窗没出现」和「模型加载不起来」变成同一行。

reader 起不来**不**结束整次 run。别的动词一概不受影响，
而且这条命令后面排的队里可能只有 capture 和 click。

### 引擎的寿命与它的 payload

`OcrTextReader` 在**第一次读**的时候才建 ONNX Runtime session，绝不在启动时。
由此得到两件事：只截图的会话完全不为它没用到的模型付账；payload 缺失的 agent 照样能启动、
照样服务其它每一个动词。创建**失败**不缓存，所以在运行中的 agent 旁边补回 payload，
下一次读就会用上；只有成功才缓存。

payload 是**相对可执行文件**解析的——`<exe dir>/models/ppocr-v6-small-rec/`——
并且没有任何 flag 指向别处，因为 agent 是 detached 启动的，它的工作目录不是它可以依赖的东西。
`entry/CMakeLists.txt` 把那个目录和 ONNX Runtime 的 DLL 摆进 runtime 输出目录，
发布也按同样方式携带。检测模型刻意不摆：没人加载它。

链接 `modules/ocr` 让 `umbra-input-agent` 多了一条对 `onnxruntime.dll` 的加载期导入。
`m0-demo` 链接同一个 support 库但没引用 ocr 模块里的任何符号，链接器把它丢掉了，
所以这个冻结的 demo 没有多出任何运行期依赖。

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
  而不是会话自己设的一个标志。它同时用一个脚本化的 `IInputAgentTextReader` 固定 `read`：
  三种答案分得开、装不下的矩形在 reader 没被碰过之前就被拒、payload 缺失被算在 reader
  头上而不是矩形头上。其中一例用真的 `OcrTextReader` 打在测试二进制旁边摆好的 payload 上，
  这是「发布路径确实能找到模型」的唯一自动化证明；至于读出了什么，
  那是 `tests/ocr/test-ocr-real.cpp` 的题目，不是这个文件的。
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
wire name，仅此而已。朝 `ocr`：`IOcrEngine` 端口、它的 `TextLine` 词汇，以及 `createOnnxEngine`
——这个程序是那个模块的组合根，截至 2026-07-31 也是它唯一的调用方。朝 `vision`：`BgraImage`，
`Frame` 在被识别之前必须被看成的那个形态。

`modules/annotation` 与 `modules/engine` 和这个程序没有链接边，理由不是分层洁癖而是领域：
标注会话手上没有已标注的东西可供授权。`entry/cli` 的 `drive` 子命令刻意在 runner adapter
一侧重新实现这套协议语义——行大小上限、fresh results 文件、输出围栏——而不是链接到这里。

## 相关

- [`entry-m0-demo.md`](entry-m0-demo.md) —— 这个程序长出来的那个冻结 demo，两者至今共用底座。
- [`entry-cli.md`](entry-cli.md) —— 产品 runner，以及它的 `drive` 协议欠这条协议的东西。
- `docs/pitfalls/capture-and-target-selection.md` —— 为什么输入走这个程序而不是手搓
  `PostMessageW`。
