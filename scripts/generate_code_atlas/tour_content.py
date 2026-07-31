# -*- coding: utf-8 -*-
"""《UmbraFlow 深度导读》的章节内容。

由 render_tour.py 渲染为 docs/knowledge/tour.html。代码摘录必须与源文件逐字一致
（render_tour.py 构建时校验），修改源码后需同步这里的摘录与行号。
"""

# 每章: num/key/title/thesis/sections/lessons
# section: h(标题) paras(段落列表) code(摘录列表) asides(补充框列表)
# 摘录: file/start/caption/code(原样,r-string)/ann([(绝对行号, 注解)])
# aside: kind = whynot | pitfall | lesson

CHAPTERS = [
{
"num": 1, "key": "problem", "title": "问题与三条红线",
"thesis": "UmbraFlow 的功能一句话就能说清楚：盯着一个游戏窗口的画面，识别当前是哪个页面，然后在后台替你点一下按钮。这一章先讲清楚它要解决的问题，以及三条贯穿整个设计的约束。后面每一章的设计决策，几乎都能追溯到这三条约束上。",
"sections": [
{"h": "它到底做什么", "paras": [
"先看完整的使用流程。你在 umbra-workbench 里打开一张游戏截图，框出一块区域，告诉它：出现这个图案就说明当前在主城页面；再框出出发按钮的位置。这些标注会被编译、发布成一套运行时资产。之后运行 umbra-flow run，它加载资产、持续截取目标窗口的画面，等到主城页面出现、找到出发按钮，就向那个窗口发送一次后台点击。",
"从功能上看，这就是常见的截图、找图、点击，用按键精灵也能拼出来。但这个项目的前提和按键精灵完全不同：程序运行的时候你还在用这台电脑，它不能动你的鼠标、不能抢焦点；它点错的代价很高，比如把出发按钮认成商店的购买按钮；另外它的识别逻辑需要在没有图形界面的 CI 机器上反复测试。这三个前提分别对应三条约束：严格后台、fail-closed、确定性。"], "code": [], "asides": []},
{"h": "红线一：严格后台", "paras": [
"先说第一条。Windows 上模拟输入通常用 SendInput 或者 mouse_event，但这两个 API 都作用于全局：它们会移动真实的鼠标指针，你正在打字就会被打断。SetForegroundWindow 更直接，把目标窗口整个切到前台来。所以这些 API 全部禁用，唯一允许的输入方式是向目标窗口的 HWND 调用 PostMessageW——消息直接进入目标窗口的消息队列，不经过全局输入状态，也不影响焦点。禁止名单直接写在 controller 的头文件里：",
"把名单写成代码而不是写在 wiki 里，有两个直接的好处：测试可以引用这个数组，逐个检查输入实现没有调用被禁 API；名单本身进了版本控制，谁想加例外，diff 里一目了然。更重要的是模块结构上的隔离：整个项目只有 controller 模块能接触 Win32 输入 API，engine 和 annotation 连相关头文件都 include 不到，想写出偷焦点的代码都没有入口。"], "code": [
{"file": "modules/controller/source/controller/input.hpp", "start": 32,
"caption": "禁止 API 名单，写成代码而不是文档",
"code": r"""    inline constexpr auto k_forbiddenBackgroundApis = std::array<std::string_view, 6>{
        "SetForegroundWindow",
        "SetFocus",
        "SendInput",
        "mouse_event",
        "keybd_event",
        "SetCursorPos",
    };""",
"ann": [(32, "constexpr 数组：测试可以直接遍历名单做检查，改动会体现在 diff 里")]}], "asides": []},
{"h": "红线二：fail-closed", "paras": [
"第二条约束管的是不确定的情况。识别类的自动化总会碰到判断不了的时刻：画面正好在过场动画上、两个页面的特征同时匹配、识别跑到一半超时了。这时有两种选择：挑一个最像的继续执行，或者停下来什么都不做。这个项目一律选后者，理由很实际：点错无法撤销，漏点最多等下一轮重试。",
"这条规则贯彻得比一般项目彻底。不只是识别不确定时不点：观察到的画面太旧不点，目标窗口疑似被换过不点，甚至运行记录写不进磁盘也不点。后面几章会看到这些检查具体发生在哪里。"], "code": [], "asides": []},
{"h": "红线三：确定性与可测试性", "paras": [
"第三条约束来自测试。CI 机器上没有桌面、没有窗口可以截图，但识别和授权恰恰是最需要反复测试的逻辑。解决办法是把代码按照是否接触平台切开：判断点不点、点哪里的逻辑（domain、vision、image、annotation、engine 五个模块）完全不依赖 Windows，在 Linux 上照常编译和测试；真正截屏、发消息的代码（controller 和 entry 里的适配器）写得尽量薄，只在 Windows 真机上验收。",
"为了让测试能做逐字节的断言，识别过程本身还必须是确定的：同样的输入永远给出同样的输出。这解释了后面会遇到的几个看起来偏执的选择——相似度计算用整数不用浮点、阈值存成 0 到 10000 的整数、资产文件按内容哈希命名。第 7 章展开讲。"], "code": [], "asides": [
{"kind": "lesson", "title": "读陌生系统时，先找它的约束", "body": "这个项目的模块划分、类型设计、错误处理方式，都能从三条约束反推出来。反过来，自己设计系统的时候，先把绝对不能发生的事列出来，再考虑用什么结构让这些事写不出来，比事后靠 code review 把关省力得多。"}]},
],
"lessons": ["先明确绝对不能发生的事，再设计让它写不出来的结构", "危险 API 集中在一个模块里，上层没有 include 的机会", "点错无法撤销、漏点可以重试，所以一切不确定都导向不做"]},

{
"num": 2, "key": "mainline", "title": "组合根：一次 run 是怎么拼起来的",
"thesis": "读这个代码库，建议从 entry/cli/run-windows.cpp 开始，而不是从最底层的 core 开始。原因很简单：平台相关代码和平台无关代码在整个项目里只在这一个文件汇合。读懂它，整个系统的结构就有了地图。",
"sections": [
{"h": "组合根模式", "paras": [
"这种把所有依赖集中在一处组装的写法叫组合根（composition root）。engine 的依赖清单里没有 controller，controller 也不知道页面、识别器这些概念，两边只在这里见面。CLI 先在不碰桌面的情况下做完所有能提前做的事：解析参数、加载运行时清单、把页面名和动作名换成内部 ID。这些步骤都可能失败，而失败都发生在创建任何平台资源之前——清单是坏的，就不会先建了捕获会话再报错。然后才是选择目标窗口、建立捕获，最后把平台能力包成 engine 需要的三个接口：",
"装配完之后，主流程只有三个调用：waitForPage 等页面出现，findAction 找按钮，act 点击。前面的几十行都是在为这三行做准备。"], "code": [
{"file": "entry/cli/run-windows.cpp", "start": 131,
"caption": "组合根：包装适配器，创建会话",
"code": r"""            // 7. Wire the adapters over the resolved capabilities.
            UF_TRY_VALUE(traceSink, FileTraceSink::create(args.trace));
            auto frameSource = std::make_unique<platform::WgcFrameSource>(
                std::move(session)
            );
            auto actionSink = std::make_unique<platform::ControllerActionSink>(
                delivery
            );

            // 8. Build the engine session over the resolved capabilities.
            auto config = engine::EngineSessionConfig{
                .liveFingerprint         = liveFingerprint,
                .maximumPixelComparisons = args.budget,
                .recognitionTimeout      = args.recognitionTimeout,
                .maxActionFrameAge       = args.maxFrameAge,
                .cancellation            = cancellation.token(),
            };
            UF_TRY_VALUE(
                engineSession,
                engine::EngineSession::create(
                    std::move(loaded),
                    std::move(frameSource),
                    std::move(actionSink),
                    std::move(traceSink),
                    std::move(config)
                )
            );""",
"ann": [
(133, "WgcFrameSource 和 ControllerActionSink 是仅有的两个平台适配器，各自只有几十行转发代码"),
(141, "指定初始化器让配置一目了然：识别预算、超时、帧龄上限、取消令牌"),
(146, "这个 stop_token 来自 Ctrl-C 处理，会一路传到识别循环深处（第 7 章）"),
(152, "三个接口用 unique_ptr 交出所有权，接口实现和其中的平台资源跟会话同生共死")]}], "asides": []},
{"h": "两个值得注意的细节", "paras": [
"主流程有两个细节。第一，findAction 是 session 的方法，必须显式传入 pageWait 中的 observation；这样既保证查找使用认出页面的同一帧，也不让 observation 反向借用 session。没找到按钮时返回空 optional，程序正常写报告退出。第二，act 的第一个参数写的是 std::move(pageWait.observation)：点击会把整个观察对象消耗掉，之后原变量就不能再用了。为什么要这样设计，是第 4 章的主题。"], "code": [
{"file": "entry/cli/run-windows.cpp", "start": 185,
"caption": "act 消耗掉整个观察对象",
"code": r"""            UF_TRY_VALUE(
                pageWait,
                bound.session.waitForPage(pageId, args.timeout, args.pollInterval)
            );
            UF_TRY_VALUE(
                maybeAction,
                bound.session.findAction(pageWait.observation, actionId)
            );

            auto report = RunReport{
                .pageName   = args.page,
                .actionName = args.action,
                .tracePath  = args.trace.string(),
            };
            if (!maybeAction)
            {
                report.actionDelivered = false;
                return report;
            }

            UF_TRY_VALUE(
                receipt,
                bound.session.act(
                    std::move(pageWait.observation),
                    pageWait.page,
                    *maybeAction
                )
            );
            report.actionDelivered = true;
            report.clickClientX    = receipt.clickPoint.x();
            report.clickClientY    = receipt.clickPoint.y();
            return report;""",
"ann": [
(191, "findAction 显式接收 observation：同帧事实清楚，但 observation 不保存 session 指针"),
(208, "move 进去之后，这个 observation 就不能再用了——编译期和运行时都有检查"),
(213, "返回的 ActReceipt 记录点击对应的帧和实际投递的坐标，用来写报告")]}], "asides": [
{"kind": "lesson", "title": "入口代码越平淡越好", "body": "run-windows.cpp 里没有一个业务判断，全是准备、组装、调用。如果哪天组合根里开始出现 if-else 业务逻辑，通常说明某个模块该管的事漏到了外面。评估一个项目的耦合程度，读它的组合根是最快的办法。"}]},
],
"lessons": ["平台相关代码只出现在组合根这一个位置，方便审计", "先做完所有离线检查，再创建第一个平台资源", "读陌生系统从组合根入手，它是全系统的地图"]},

{
"num": 3, "key": "ports", "title": "端口：engine 与平台之间的三个接口",
"thesis": "engine 需要三种平台能力：截一帧画面、投一次点击、写一条记录。它对平台的全部了解就是三个纯虚基类，加起来不到一百行。这一章逐个看这三个接口。重点其实是接口上的注释——它们写的不是每个方法做什么，而是接口为什么长成这样。",
"sections": [
{"h": "IFrameSource：截图，加一个看似多余的方法", "paras": [
"IFrameSource 有两个方法。capture 返回一帧画面，没什么可说的。validateTargetInstance 检查绑定的还是不是原来那个窗口——乍一看多余，capture 失败不也能发现窗口没了吗？它单独存在的原因要到第 5 章才完全展开：在点击发出前的最后一刻，需要一次不截图的快速复核。注释里还写明了这个接口预留的两个扩展点：将来的非 Windows 平台，以及 CI 里回放固定画面序列的测试替身。"], "code": [
{"file": "modules/engine/source/engine/ports.hpp", "start": 13,
"caption": "IFrameSource：注释里写清了接口存在的理由",
"code": r"""    // A port over ONE bound capture target. Modeled on the surface of the
    // Windows WgcCaptureSession so the platform adapter is a thin wrapper that
    // forwards capture() and revalidates the bound target instance.
    //
    // Seams:
    //  - P3 second platform: a non-Windows adapter implements the same two
    //    methods; nothing above this port is platform-aware.
    //  - Tests: a fake replays a fixed vector<Frame> for CI without a live
    //    desktop, so the engine can be exercised deterministically.
    class IFrameSource
    {
    public:
        IFrameSource() = default;

        IFrameSource(IFrameSource const&) = delete;
        IFrameSource(IFrameSource&&) = delete;
        auto operator=(IFrameSource const&) -> IFrameSource& = delete;
        auto operator=(IFrameSource&&) -> IFrameSource& = delete;

        virtual ~IFrameSource() = default;

        [[nodiscard]] virtual auto capture() -> Result<Frame> = 0;
        [[nodiscard]] virtual auto validateTargetInstance() -> Status = 0;
    };""",
"ann": [
(13, "ONE bound capture target：接口建模的是一个已经绑定好的目标，选窗口不是 engine 的事"),
(27, "拷贝和移动都删掉了：实现只能被 unique_ptr 独占，不会出现指向平台资源的第二个引用"),
(35, "validateTargetInstance 单独成方法，因为复核目标和截图的调用时机不同（第 5 章）")]}], "asides": []},
{"h": "IActionSink：把安全要求写进参数列表", "paras": [
"IActionSink 的注释是三个接口里语气最重的。click 除了坐标还要接收一个 lease（观察租约，第 5 章的主角），并且注释用大写的 MUST 要求实现把它原样传给投递层。为什么点击接口要带租约？因为授权分两层：engine 这边授权完是第一层，controller 在真正发消息之前，还要用 lease 里的帧身份和年龄再独立查一遍，这是第二层。如果接口只收坐标，写适配器的人少传一个参数，第二层检查就悄悄消失了，而且没有任何报错。把 lease 写进函数签名之后，这个错误就没法犯了。"], "code": [
{"file": "modules/engine/source/engine/ports.hpp", "start": 38,
"caption": "IActionSink：lease 必须原样传递，这是接口的一部分",
"code": r"""    // A port that delivers a single background click to the bound target. The
    // engine has already authorized the coordinate (layer 1) by the time click()
    // is called, but that authorization is only the first of two checks. The
    // implementation MUST also forward the lease to the delivery layer so the
    // controller's D0 injection-layer fencing -- frameId, targetGeneration, and
    // age revalidation performed at delivery time -- stays in the loop as layer 2.
    // Dropping the lease would silently remove that security-reviewed second
    // check. The implementation MUST additionally revalidate the target identity
    // before posting and MUST deliver strictly in the background: it never steals
    // focus and never activates the target window.
    class IActionSink
    {
    public:
        IActionSink() = default;

        IActionSink(IActionSink const&) = delete;
        IActionSink(IActionSink&&) = delete;
        auto operator=(IActionSink const&) -> IActionSink& = delete;
        auto operator=(IActionSink&&) -> IActionSink& = delete;

        virtual ~IActionSink() = default;

        [[nodiscard]]
        virtual auto click(
            Point<ClientSpace> point,
            ObservationLease const& lease
        ) -> Status = 0;
    };""",
"ann": [
(44, "注释直接点破了风险：丢掉 lease 会静默移除一道安全评审过的检查"),
(62, "坐标类型是 Point<ClientSpace>，传一个没换算过的帧坐标进来编译不过（第 6 章）"),
(63, "lease 全程按 const 引用传递，投递层拿到的和授权层看到的是同一份事实")]}], "asides": [
{"kind": "whynot", "title": "为什么不用 #ifdef 或者模板？", "body": "用 #ifdef 隔离平台代码，Win32 类型还是会出现在 engine 的头文件里，Linux CI 直接编译不过。用模板参数注入平台实现（policy-based design），engine 的每个实例化都绑死一种平台类型，测试替身和真实实现很难在同一个测试二进制里共存。这里用虚函数没有性能顾虑——这些接口一帧才调用一次——而运行时可替换恰恰是测试需要的。选抽象手段要看调用频率和替换需求，不看哪种写法流行。"}]},
{"h": "ITraceSink：写记录也可能失败", "paras": [
"第三个接口 ITraceSink 只有一个 emit 方法，但注意它的返回值是 Status：写记录可能失败，而且失败会中止当前操作，不是打条警告继续跑。注释给了理由：可追溯性是这个系统承重的约束之一，所以 engine 在错误发生的那一刻就记录，赶在任何上层调用者有机会吞掉错误之前。这条证据链的完整设计放在第 8 章。"], "code": [
{"file": "modules/engine/source/engine/ports.hpp", "start": 67,
"caption": "ITraceSink：记录失败是错误，不是警告",
"code": r"""    // A port that records one trace event. Traceability is a load-bearing
    // constraint, so an emit failure is an error rather than a best-effort
    // side effect (D4): the engine emits at the throw-instant, before any caller
    // can swallow the failure it describes.
    class ITraceSink
    {
    public:
        ITraceSink() = default;

        ITraceSink(ITraceSink const&) = delete;
        ITraceSink(ITraceSink&&) = delete;
        auto operator=(ITraceSink const&) -> ITraceSink& = delete;
        auto operator=(ITraceSink&&) -> ITraceSink& = delete;

        virtual ~ITraceSink() = default;

        [[nodiscard]] virtual auto emit(TraceEvent const& event) -> Status = 0;
    };""",
"ann": [(67, "对一个替人点击的程序来说，做了什么说不清，跟做错了一样严重")]}], "asides": []},
],
"lessons": ["接口按能力划分，不按平台划分", "把安全要求写进参数列表，实现想偷懒也绕不过去", "接口注释写为什么，而不是重复方法名"]},

{
"num": 4, "key": "observation", "title": "Observation：一次观察是一个对象",
"thesis": "engine 有三条使用规矩：识别和点击必须基于同一帧画面；一次观察最多点一次；点完必须重新观察。这些规矩没有写成文档让调用者自觉遵守，而是做成了一个类型。这一章看 Observation 是怎么把规矩变成编译错误的。",
"sections": [
{"h": "先看朴素设计的问题", "paras": [
"假设按最直接的方式设计 API：capture 截图，recognize 识别并返回坐标，click 点击，三个独立函数。每个函数单独看都没问题，组合起来问题就来了。你可以在 recognize 之后又调一次 capture，然后用旧坐标去 click——坐标是上一帧算出来的，画面已经变了，点击落在新画面的错误位置上。你可以拿同一个坐标 click 两次。在多会话的场景下，你甚至可以把 A 会话识别出的坐标发给 B 会话。这些错误编译器全都不拦，测试也很难覆盖到，只能指望使用者小心。",
"Observation 的思路是把一次观察变成一个对象：画面、租约、身份信息都装在里面，由 EngineSession::observe 统一发出。识别页面或找按钮时，session 必须显式接收这个 observation，因此读取的自然是同一帧画面，同时 observation 不需要反向借用 session；想点击，必须把整个对象 move 进 act，之后原变量就不能再用了。"], "code": [
{"file": "modules/engine/source/engine/session.hpp", "start": 77,
"caption": "Observation 的完整声明：单次句柄与稳定 session identity",
"code": r"""    // A single-use, move-only handle over one captured frame, vended only by
    // EngineSession::observe. It carries the frame, its lease, its frame
    // identity, and a shared immutable token identifying the session that
    // produced it.
    //
    // The token owns no session state and is never dereferenced. It follows a
    // moved EngineSession and lets every operation reject a handle vended by a
    // different session without retaining a borrow into the session object.
    // Consuming the handle by value makes it typed single-use, and the invalidated
    // flag fences any surviving alias at runtime. The move operations copy the
    // members into the destination and invalidate the source, so a moved-from
    // handle is dead exactly like a consumed one and fails StaleObservation on
    // any later use.
    class Observation final
    {
        friend class EngineSession;

        Frame                                                m_frame;
        ObservationLease                                     m_lease;
        annotation::FrameIdentity                            m_frameIdentity;
        std::shared_ptr<detail::EngineSessionIdentity const> m_sessionIdentity;
        bool                                                 m_invalidated{false};

        Observation(
            Frame frame,
            ObservationLease lease,
            annotation::FrameIdentity frameIdentity,
            std::shared_ptr<detail::EngineSessionIdentity const> sessionIdentity
        ) noexcept;

    public:
        Observation(Observation const&) = delete;
        Observation(Observation&& other) noexcept;
        auto operator=(Observation const&) -> Observation& = delete;
        auto operator=(Observation&& other) noexcept -> Observation&;

        ~Observation() = default;

    };""",
"ann": [
(83, "token 会跟随 moved session，不会留下指向 moved-from 对象的悬空 borrow"),
(92, "构造函数私有，加 friend：只有 EngineSession::observe 能创建观察对象"),
(97, "shared identity 只用于比较出处，不拥有、也不解引用 session 状态"),
(108, "拷贝构造删除：两个句柄指向同一帧画面，就意味着可能点两次"),
(109, "移动构造会把源对象标记为失效，moved-from 的句柄和用过的句柄行为完全一样")]}], "asides": []},
{"h": "类型挡住大多数，标志位兜底", "paras": [
"act 按值消耗句柄之后，大多数误用在编译期就结束了。但 C++ 的 move 不是真正的线性类型：moved-from 的对象还在，如果别处还留着引用，仍然可能被再次传给 session。所以 resolvePage、findAction 和 act 进来的第一件事都是检查 m_invalidated 标志，发现句柄已经用过就返回 StaleObservation 错误。两道防线配合下来：编译器拦住绝大多数误用，标志位把剩下的变成一个确定的运行时错误。中间没有碰巧还能用的灰色地带。",
"还有一个细节值得注意：move 在这里被赋予了业务含义。移动即失效，不是资源管理的副产品，而是协议本身的一部分。第 5 章会看到 act 内部在点击落地之后手动设置这个标志的精确时机——那是防止重复点击的最后一环。"], "code": [], "asides": [
{"kind": "lesson", "title": "C++ 里的单次使用协议", "body": "Rust 的所有权系统可以在语言层面表达用过之后不能再用。C++ 做不到，但 move-only 类型、按值消费的参数、一个失效标志，三样合起来能达到九成的效果，成本只是一个 bool。任何有打开、使用、关闭这类生命周期的资源都可以套用这个做法。"}]},
],
"lessons": ["API 的职责是让误用写不出来，而不是在文档里提醒", "私有构造加 friend，控制对象只能从合法途径产生", "move-only 加按值消费加失效标志，就是 C++ 版的一次性句柄"]},

{
"num": 5, "key": "gates", "title": "一次点击要过几道门",
"thesis": "act 函数是整个系统安全设计的汇合点：从进入函数到 PostMessageW 发出，要过九道检查，任何一道不过，这次点击就取消。这一章按顺序走一遍，最后停在一行特别重要的赋值语句上。",
"sections": [
{"h": "前三道门", "paras": [
"前三道检查和识别完全无关：有没有人请求取消、这个观察对象是不是本会话发出的、它是不是已经被用过。代码里的注释有个细节值得注意：它解释的不是每个检查做什么——代码本身够清楚——而是为什么取消检查可以排在最前面（它只读会话自己的状态，不需要碰传进来的观察对象，所以不必等出处检查先做完）。这种解释顺序的注释，在这个代码库里随处可见。"], "code": [
{"file": "modules/engine/source/engine/session.cpp", "start": 443,
"caption": "act 的前三道检查：取消、出处、是否已用过",
"code": r"""        // An external stop requested before delivery takes precedence over every
        // other outcome: fail closed before authorization and any sink call so a
        // cancelled run never posts input. This reads only session state, not the
        // observation, so it may run ahead of the foreign-observation guard below.
        if (m_config.cancellation.stop_requested())
        {
            return fail(
                AutomationErrorKind::Cancelled,
                "cancelled before delivery"
            );
        }

        // D0: an observation carries the stable identity token of the session
        // that vended it. Acting on a handle from another session is a programming
        // error, so reject it before any other check touches the foreign handle.
        if (observation.m_sessionIdentity != m_identity)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "observation belongs to a different session"
            );
        }

        if (observation.m_invalidated)
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "act called on an invalidated observation"
            );
        }""",
"ann": [
(447, "已经请求取消的运行绝不投递输入"),
(458, "拿别的会话的观察来点击被归为程序错误（InternalInvariant），和可恢复的运行时错误分开"),
(466, "第 4 章的失效标志，在这里第一次被用作守卫")]}], "asides": []},
{"h": "第一层授权", "paras": [
"过了前三道门，engine 从观察对象里取出帧身份，交给 annotation 模块授权。授权函数的参数列表本身就是一份检查清单：识别目录（查这个动作允许出现在哪些页面上）、已解析的页面（证据一）、动作检测结果（证据二）、观察租约（保证画面够新鲜）、当前投递状态（当前项目指纹和当前时间）。核心的比对是身份：页面证据、动作证据、即将执行的点击，三者的 CaptureSessionId、TargetGeneration、FrameId 必须完全一致——换句话说，它们必须来自同一个捕获会话、同一代目标窗口、同一帧画面。",
"租约是这里反复出现的概念，值得停下来看清楚它的结构。四个字段：三个身份 ID，加一个过期时刻。它只能通过 forFrame 从一帧真实画面构造出来，没有办法凭空捏造；validate 方法把四项全部比对一遍，任何一项不符就返回 StaleObservation。授权失败的时候还要注意代码顺序：先构造一条 ActionRejected 记录写出去，写成功了才把错误返回给调用者。错误在哪里发生就在哪里记录，不指望上层调用者记得写日志。"], "code": [
{"file": "modules/annotation/source/annotation/authorization.hpp", "start": 46,
"caption": "授权函数的参数列表就是检查清单",
"code": r"""    struct ActionDeliveryState final
    {
        ProjectFingerprint liveFingerprint;
        CaptureSessionId   sessionId;
        TargetGeneration   targetGeneration{};
        FrameId            frameId;
        MonotonicInstant   now;
    };

    [[nodiscard]]
    auto authorizeCoordinateAction(
        RecognitionCatalog const& catalog,
        ResolvedPage const& resolvedPage,
        ActionDetection const& actionDetection,
        ObservationLease const& lease,
        ActionDeliveryState delivery
    ) -> Status;""",
"ann": [
(48, "运行时的项目指纹必须和资产目录的指纹一致，防止加载着 A 项目却按 B 项目的规则点击"),
(58, "参数类型只收 ResolvedPage：页面识别结果为未知或有歧义时，在类型上就进不了授权（第 7 章）")]},
{"file": "modules/domain/source/domain/detection.hpp", "start": 50,
"caption": "ObservationLease：三个身份 ID 加一个过期时刻",
"code": r"""    class ObservationLease final
    {
        CaptureSessionId m_sessionId;
        TargetGeneration m_targetGeneration;
        FrameId          m_frameId;
        MonotonicInstant m_expiresAt;

        constexpr ObservationLease(
            CaptureSessionId sessionId,
            TargetGeneration targetGeneration,
            FrameId frameId,
            MonotonicInstant expiresAt
        ) noexcept
            : m_sessionId{sessionId}
            , m_targetGeneration{targetGeneration}
            , m_frameId{frameId}
            , m_expiresAt{expiresAt}
        {
        }

    public:
        auto operator==(ObservationLease const&) const -> bool = default;

        [[nodiscard]]
        static auto forFrame(
            Frame const& frame,
            MonotonicInstant::Duration maximumAge
        ) -> Result<ObservationLease>;

        [[nodiscard]] auto sessionId() const noexcept -> CaptureSessionId;
        [[nodiscard]] auto targetGeneration() const noexcept -> TargetGeneration;
        [[nodiscard]] auto frameId() const noexcept -> FrameId;
        [[nodiscard]] auto expiresAt() const noexcept -> MonotonicInstant;

        [[nodiscard]] auto isExpired(MonotonicInstant now) const noexcept -> bool;

        [[nodiscard]]
        auto validate(
            CaptureSessionId currentSession,
            TargetGeneration currentGeneration,
            FrameId observedFrame,
            MonotonicInstant now
        ) const -> Status;
    };""",
"ann": [
(52, "CaptureSessionId 区分捕获会话；TargetGeneration 在窗口实例、句柄或客户区尺寸变化时加一；FrameId 逐帧递增"),
(55, "租约有过期时间：画面证据是会过期的，太旧的观察不能拿来兑换点击"),
(73, "构造函数私有，只能通过 forFrame 从真实的帧构造，凭空造不出有效租约"),
(87, "validate 四项全比，任何一项不符就返回 StaleObservation")]}], "asides": []},
{"h": "投递边缘，和最重要的一行", "paras": [
"授权通过之后还剩三步。第一步，把点击位置从模板像素坐标换算到帧坐标，再换算到窗口客户区坐标。注意换算用的是捕获那一帧时保存下来的几何信息，不是当前时刻的窗口位置——决策依据的一切都来自同一帧。第二步，在调用 click 之前的最后一刻，再调一次 validateTargetInstance。从观察到现在过了几十毫秒，这期间窗口可能被关掉，HWND 甚至可能被系统回收再分配给别的程序；这次复核就是为了堵上这个时间窗。第三步才是真正的 click，lease 跟着参数传下去，controller 在发消息之前用它做第二层独立校验（checkPointerPreconditions 会重查会话、代际、租约年龄和坐标范围）。",
"然后是整个函数最关键的一行：observation.m_invalidated = true，位置在 click 之后、两次 trace 写出之前。为什么必须是这个顺序？点击此刻已经发出去了，收不回来。如果接下来 ClickDelivered 记录写失败，act 会返回错误——但因为句柄已经先失效了，调用者就算还留着引用去重试，也只会得到 StaleObservation。反过来，如果先写记录再置失效标志，写记录失败看起来就和点击失败一模一样，调用者一重试，就是第二次点击。一行赋值放在哪里，决定了最多点一次这个承诺是否成立。"], "code": [
{"file": "modules/engine/source/engine/session.cpp", "start": 499,
"caption": "投递前的最后复核，点击，然后立刻让句柄失效",
"code": r"""        UF_TRY(emit(identityEvent(TraceEventKind::ActionAuthorized, identity)));

        UF_TRY_VALUE(framePoint, pixelPointToFramePoint(action.clickPixel()));
        auto const clientPoint = observation.m_frame.transform().frameToClient(framePoint);

        // Revalidate the bound target instance at the delivery edge, immediately
        // before the sink call, to close the HWND-reuse window between observation
        // and delivery. This runs for every adapter, so a target replaced after
        // authorization is rejected here with zero sink calls.
        auto revalidation = m_frameSource->validateTargetInstance();
        if (!revalidation)
        {
            auto event         = identityEvent(TraceEventKind::ActionRejected, identity);
            event.errorKind    = automationErrorKind(revalidation.error());
            event.elementId = action.actionDetection().elementId();
            event.message      = std::string{revalidation.error().message()};
            UF_TRY(emit(event));
            return std::unexpected{std::move(revalidation).error()};
        }

        // Forward the lease so the delivery layer re-runs the D0 injection-layer
        // fence (frameId, targetGeneration, and age) at post time as layer 2.
        UF_TRY(m_actionSink->click(clientPoint, observation.m_lease));

        // D0/D1: the click has landed, so consume the handle before any fallible
        // post-click trace emit. If a ClickDelivered or ObservationInvalidated
        // emit then fails, the error still propagates, but a retry with a
        // surviving alias finds the handle already dead and cannot double-deliver.
        observation.m_invalidated = true;""",
"ann": [
(501, "坐标换算用的是那一帧自带的变换信息，不是当前的窗口位置"),
(508, "第 3 章说的第二次 validateTargetInstance 就在这里：目标被换掉的话，click 一次都不会被调用"),
(521, "click 带着 lease，controller 的第二层校验从这里开始"),
(527, "关键的一行：点击已经发出，先让句柄失效，再做后面可能失败的记录。顺序反了就有双击风险")]}], "asides": [
{"kind": "pitfall", "title": "act 返回错误，不代表没点击", "body": "act 返回错误有两种情况：点击前的某道检查没过（确实没点），或者点击已经发出、之后的记录写失败了（点了）。所以调用者不能用返回了错误来推断可以放心重试。engine 保证了误重试不会造成第二次点击，但理解这层语义仍然是使用者的责任。"}]},
],
"lessons": ["检查的先后顺序是设计的一部分，用注释说明理由", "两层校验各拿各的输入，不信任上一层查过了", "副作用一旦发生，先把防重入的状态置好，再做后面可能失败的事"]},

{
"num": 6, "key": "types", "title": "类型防线：让传错参数无法编译",
"thesis": "这一章讲 core 和 domain 里三个成本很低的类型技巧：带标签的坐标、强类型 ID、写进返回值的错误通道。它们对付的是同一类问题——参数传错了，编译器却不吭声。",
"sections": [
{"h": "四种坐标，一个标签", "paras": [
"系统里同时存在四种坐标：桌面坐标、窗口客户区坐标、捕获帧坐标、归一化坐标。它们都是两个 float，混着用编译器不会有任何意见，运行时的表现就是点错位置。传统自动化脚本最常见的 bug 就出在这里：截图是按帧算的，点击是按屏幕发的，中间忘了换算。解决办法是给 Point 加一个表示坐标系的模板参数：",
"Space 是个空结构体，编译完就没了：Point<FrameSpace> 的内存布局和两个裸 float 完全一样，没有任何运行时开销。但在类型系统里，Point<FrameSpace> 和 Point<ClientSpace> 是两个不相干的类型。回头看第 5 章的换算链：pixelPointToFramePoint 返回帧坐标，frameToClient 返回客户区坐标，IActionSink::click 只收 Point<ClientSpace>。想把没换算过的帧坐标直接传给点击，编译不过。"], "code": [
{"file": "modules/domain/source/domain/space.hpp", "start": 13,
"caption": "空标签结构体，加一个封闭合法取值的 concept",
"code": r"""    struct DesktopSpace final
    {
    };

    struct ClientSpace final
    {
    };

    struct FrameSpace final
    {
    };

    struct NormalizedSpace final
    {
    };

    template <typename Space>
    concept CoordinateSpace = (
        std::same_as<Space, DesktopSpace>
        || std::same_as<Space, ClientSpace>
        || std::same_as<Space, FrameSpace>
        || std::same_as<Space, NormalizedSpace>
    );

    template <CoordinateSpace Space>
    class Point final
    {
        float m_x;
        float m_y;

    public:
        constexpr Point(float x, float y) noexcept
            : m_x{x}
            , m_y{y}
        {
        }

        auto operator==(Point const&) const -> bool = default;

        [[nodiscard]] constexpr auto x() const noexcept -> float { return m_x; }
        [[nodiscard]] constexpr auto y() const noexcept -> float { return m_y; }
    };""",
"ann": [
(29, "concept 把合法的坐标系收成白名单，外部代码造不出 Point<自己编的标签>"),
(37, "标签只在编译期存在，Point 的大小就是两个 float"),
(50, "同坐标系的点可以比较，跨坐标系的比较连重载决议都过不去")]}], "asides": []},
{"h": "ID 也有同样的问题", "paras": [
"CaptureSessionId、FrameId、TargetGeneration 底层都是整数，而第 5 章的授权恰恰要在这三个数之间做比对。如果它们都用 uint64，把参数顺序传反的代码不但能编译，多数测试还发现不了——测试场景里这几个值经常恰好相等。等真出问题的时候，就是授权拿错值做比对、放行了不该放行的点击。StrongId 用空标签类型把每种 ID 隔成独立类型，传反了直接编译报错："], "code": [
{"file": "modules/domain/source/domain/ids.hpp", "start": 16,
"caption": "一行一个身份，标签不同，实现共享",
"code": r"""    namespace detail
    {
        struct EngineRunIdTag;
        struct TaskRunIdTag;
        struct CaptureSessionIdTag;
        struct FrameIdTag;
        struct StateIdTag;
        struct RecognitionIdTag;
        struct ActionIdTag;
        struct TargetGenerationTag;
    }

    using EngineRunId = StrongId<detail::EngineRunIdTag>;
    using TaskRunId = StrongId<detail::TaskRunIdTag>;
    using CaptureSessionId = StrongId<detail::CaptureSessionIdTag>;
    using FrameId = StrongId<detail::FrameIdTag>;
    using StateId = StrongId<detail::StateIdTag>;
    using RecognitionId = StrongId<detail::RecognitionIdTag>;
    using ActionId = StrongId<detail::ActionIdTag>;""",
"ann": [
(18, "标签类型只需要前置声明，永远不用定义，它们的全部作用就是让类型互不相同"),
(28, "新增一种 ID 的成本是两行代码；一次 ID 混用事故的排查成本远不止于此")]}], "asides": []},
{"h": "错误通道写进返回类型", "paras": [
"错误处理方面，这个项目不用异常传递业务错误，用的是 C++23 的 std::expected：函数返回 Result<T>，要么是值，要么是带出错位置和上下文链的 Error 对象。所有 Result 都标了 [[nodiscard]]，拿到返回值不检查，编译器直接警告。UF_TRY 宏负责失败就向上返回这个最常见的模式，让成功路径的代码保持一条直线——第 2 章组合根里一行一个 UF_TRY_VALUE 的写法就是这么来的。",
"UF_TRY 的定义上方有一段注释，解释宏里为什么用 auto 按值接住结果，而不是看起来能省一次移动的 auto&&：auto&& 只能延长纯右值的生命周期，如果表达式返回的是指向临时对象内部的引用，宏后面就会读到已经析构的内存。一个五行的宏，配了五行的生命周期分析。写宏就该有这种谨慎。"], "code": [
{"file": "modules/core/source/core/error/result.hpp", "start": 13,
"caption": "错误模型的核心就是两个别名",
"code": r"""    template <typename Value>
    using Result = std::expected<Value, Error>;

    using Status = Result<void>;""",
"ann": [(14, "直接用 C++23 的 std::expected，项目只补充 Error 载荷和传播宏，不自研 Result 类")]},
{"file": "modules/core/source/core/error/result.hpp", "start": 46,
"caption": "UF_TRY，以及它为什么按值持有结果",
"code": r"""// The result is held by value. Binding it to auto&& instead would only extend
// the lifetime of a prvalue, so an expression yielding a reference into a
// temporary would leave the macro reading freed storage; holding the value
// makes that unrepresentable. Error is move-only, so an lvalue operand must be
// moved in by the caller.
#define UF_TRY(expression) \
    do \
    { \
        auto ufResult = (expression); \
        if (!ufResult) \
        { \
            return ::std::unexpected{::std::move(ufResult).error()}; \
        } \
    } while (false)""",
"ann": [
(46, "注释回答的问题是：为什么不用看起来更高效的 auto&&——因为那是一个真实存在的悬垂引用陷阱"),
(51, "do while(false) 是老办法，但确实有必要：让宏在 if else 里表现得像一条普通语句")]}], "asides": [
{"kind": "lesson", "title": "强类型是最便宜的静态检查", "body": "空标签结构体、using 别名、一个宏，实现成本都只有几行，但每一样都消灭了一类过去只能靠人眼 review 的错误。其中最容易引入现有项目的是强类型 ID：任何有两种以上整数 ID 满天飞的代码库都值得做，而且今天就可以开始。"}]},
],
"lessons": ["幽灵标签参数：零运行时开销，把语义差异变成类型差异", "concept 白名单封闭合法类型的集合", "错误进返回类型加 [[nodiscard]]，漏检查在编译期就暴露"]},

{
"num": 7, "key": "recognition", "title": "有界识别：预算、停止原因与确定性",
"thesis": "识别本质上是在画面里搜索模板，而搜索必须有上限，不能让一次识别把 CPU 吃满或者迟迟不返回。有了上限就有了新问题：搜到一半停下来，算什么结果？这一章看 vision 模块怎么区分没找到和没搜完，以及整条识别链路为什么能做到完全确定。",
"sections": [
{"h": "停止原因是一等公民", "paras": [
"这个项目的模板匹配用的是 SAD（Sum of Absolute Differences）：把模板叠在画面的每个候选位置上，逐像素求灰度差的绝对值再累加，和越小越像。实现从函数签名开始就承认搜索可能中途停止：调用者给出最大像素比较次数，匹配器每比较 4096 次就轮询一次外部信号，看是否被取消或者超时。可能的停止原因用枚举一个个列出来：",
"关键在返回类型上。outcome 是一个 variant：要么搜索完整跑完（其中再分找到和没找到），要么因为某个原因提前停了。三种情况在类型上互斥，不存在既算 miss 又算超时的含糊值。engine 拿到停止原因后分别处理：预算耗尽、超时、取消映射成不同的错误；而跑完了但没找到走的是成功路径里的空 optional。第 2 章 CLI 对 maybeAction 的处理，就是这个区分传到最上层的样子。反过来想，如果这里把预算耗尽也当成没找到，上层就会把一次没搜完的搜索当成确凿的否定，然后在错误的判断上继续往下执行——这正是 fail-closed 要防的事。"], "code": [
{"file": "modules/vision/source/vision/sad.hpp", "start": 18,
"caption": "搜索控制与停止原因，各自是显式的枚举",
"code": r"""    enum class SadSearchControl : uint8
    {
        Continue,
        Cancelled,
        TimedOut,
    };

    enum class SadSearchStopReason : uint8
    {
        Cancelled,
        TimedOut,
        ComparisonBudgetExhausted,
    };

    inline constexpr auto k_sadSearchPollIntervalComparisons = uint64{4096};

    // Invoked synchronously during matching and never retained by the matcher.
    using SadSearchPoll = std::function<SadSearchControl()>;""",
"ann": [
(25, "三种停止原因分开命名，上游据此决定是重试、放弃还是报告用户"),
(32, "轮询间隔是常量不是魔数，取消响应的延迟上界可以推算出来"),
(34, "注释承诺回调不被保存——这个代码库对存储借用一贯警惕")]},
{"file": "modules/vision/source/vision/sad.hpp", "start": 62,
"caption": "结果类型：三种状态互斥，成本核算精确",
"code": r"""    using SadSearchOutcome = std::variant<
        std::optional<SadMatch>,
        SadSearchStopReason
    >;

    struct SadSearchReport final
    {
        SadSearchOutcome outcome{};

        // Counts comparisons actually executed across every candidate, including
        // the comparisons that trigger pruning or an exact-match return. A budget
        // or poll stop excludes the comparison that was not executed. Valid for
        // every outcome and starts at zero for each matcher call.
        uint64 completedPixelComparisons{};
    };""",
"ann": [
(62, "variant 套 optional：跑完没找到、跑完找到了、中途停止，三种状态在类型上分开"),
(71, "注释把计数语义精确到没执行的那一次不算，golden 测试可以对成本做精确断言")]}], "asides": []},
{"h": "确定性靠三件事", "paras": [
"要让识别结果能在 CI 里做精确断言，整条链路坚持三件事。第一，整数运算：SAD 分数是 uint64，灰度转换也是整数规则。浮点在不同编译器、不同 SIMD 路径下最后一位会有差异，golden 测试就没法写。第二，固定顺序：候选排序、页面遍历、序列化时的字段顺序全部固定，不依赖 unordered 容器的迭代顺序。第三，处处设上限：清单文件最大 16 MiB，模板最大 64 MiB，比较次数、等待时长、重试次数都有明确的顶。",
"阈值的存法是这套思路的一个缩影：不存 0.97 这样的浮点数，存 0 到 10000 的整数（基点），判断命中用带等号的整数比较。多像才算命中这个问题，在任何机器上的答案都一样。",
"资产管理也为确定性服务。模板从源图裁出来之后做规范化的 PNG 编码，用编码后字节的哈希做文件名，也就是内容寻址；运行时只加载清单里列出的文件，不扫描目录去猜。发布的那一刻整个资产闭包就固定了，之后改动任何一个字节，加载时哈希对不上，直接拒绝。"], "code": [], "asides": [
{"kind": "whynot", "title": "为什么不用 OpenCV？", "body": "不是造轮子情结。OpenCV 的模板匹配用浮点归一化互相关，不同构建（SIMD 派发、fast-math 开关）下结果不是位级一致的，golden 断言写不了；它也不提供比较预算、中途取消、停止原因这一组语义，而 fail-closed 恰恰建立在这些语义上。当一个库满足不了你最核心的性质（这里是确定性和有界性）时，两百行的专用实现比适配一个大库更省事。"}]},
],
"lessons": ["把搜索为什么停下来建模成一等的值，别让 miss 替它背锅", "确定性等于整数运算加固定顺序加显式上限", "清单是唯一权威，内容寻址让发布了什么和加载了什么永远一致"]},

{
"num": 8, "key": "evidence", "title": "证据链、测试，与可以带走的东西",
"thesis": "最后一章讲两件事：这个系统怎么记录自己做过什么，以及前面几章的约束是怎么被测试固定下来的。结尾把散在各章的经验收成一张清单。",
"sections": [
{"h": "证据链的三条纪律", "paras": [
"运行记录（trace）在第 3 章露过面：写不进去就是错误，不是警告。围绕它还有三条纪律。第一，在失败现场记录。识别出错、授权被拒、投递前复核失败，代码都是先写记录、写成功了才把错误往上抛，不指望调用链上层的某个人记得写日志。第二，磁盘格式被钉死。每条记录先写 schema 版本号，字段顺序固定，事件在磁盘上的名字由一个显式的 switch 维护——就算有人重命名了 C++ 枚举值，磁盘格式也不会跟着变，golden 测试会先报警。第三，职责分层。engine 的序列化器只负责产出单行 JSON，不碰文件；CLI 里的 FileTraceSink 负责加换行、每条立刻 flush，让程序崩溃之前已经产生的记录尽量落盘。",
"文档里同样明确写了这条证据链现在没覆盖的地方：会话创建之前的加载错误、waitForPage 自己的超时，目前不产生统一的 Failure 事件。要扩展错误路径，得去读具体的记录点，不能假设有个中央拦截器兜着。把哪里没做写清楚，和把哪里做了写清楚一样重要。"], "code": [], "asides": []},
{"h": "测试是约束的可执行形态", "paras": [
"前面几章的约束在 tests/ 里都有对应的断言，挑几个有代表性的。正常流程的测试精确断言事件序列：start、observe、page、action、authorize、click、invalidate，顺序一个不能错，而且点击接口恰好被调用一次、租约里的 FrameId 原样到达。每种拒绝路径——指纹不匹配、租约过期、页面不允许这个动作——都断言点击次数为零，外加留下正确的拒绝记录。最讲究的一个测试：故意让 ClickDelivered 的写出失败，然后断言观察句柄此时已经失效、重试只能拿到 StaleObservation。第 5 章那行赋值的位置，就是被这个测试永久固定住的。",
"测试的边界也交代得很诚实：engine 用三个替身测状态机，controller 的测试钉禁止 API 和消息编码，而 WGC 捕获、DPI、UIPI 这些只有真机才能验证的东西，被明确排除在合成测试的证明范围之外，单独列在真机验收清单里。合成测试全绿不等于系统可用，这句话写在文档里，不是留给读者自己悟。"], "code": [], "asides": [
{"kind": "lesson", "title": "把不变量翻译成测试的三个模板", "body": "一，顺序断言：把必须先 A 后 B 写成对事件序列的精确匹配。二，零副作用断言：每条拒绝路径都数一遍投递接口的调用次数，必须是零。三，故障注入：让基础设施（这里是 trace）在最刁钻的时机失败，断言安全性质还成立。这三个模板能覆盖大部分时序类的不变量。"}]},
{"h": "十条可以带走的经验", "paras": [
"最后把散在各章的经验收拢一下。它们没有一条依赖游戏自动化这个具体领域：",
"一、先写下绝对不能发生的事，再让架构使它写不出来（第 1 章）。二、平台相关代码集中在组合根一个位置（第 2 章）。三、把安全要求写进函数签名，而不是文档（第 3 章）。四、用 move-only 类型表达一次性资源的使用协议（第 4 章）。五、检查的先后顺序是设计的一部分，用注释说明理由、用测试固定（第 5 章）。六、多层校验各拿各的输入，不信任上一层查过了（第 5 章）。七、强类型 ID 和坐标标签，几行代码消灭一类传参错误（第 6 章）。八、错误通道写进返回类型，配 [[nodiscard]]，漏检查藏不住（第 6 章）。九、取消、超时、预算耗尽是三种不同的事实，不要都当成没找到（第 7 章）。十、副作用一旦发生，先把防重入的状态置好，再做后面可能失败的事（第 5、8 章）。"], "code": [], "asides": []},
],
"lessons": ["错误在发生现场记录，不指望上层自觉", "磁盘格式由显式 switch 维护，重命名枚举不会静默改变它", "诚实标注覆盖边界：合成测试全绿不等于真机可用"]},
]
