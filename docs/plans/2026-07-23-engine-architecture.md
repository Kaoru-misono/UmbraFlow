# 完成标注系统 + 产品最终架构（modules/engine）

> 状态(2026-07-24):Phase 1–4 已实现并提交(engine 模块、`umbra-flow run` 组合根、
> ImGui workbench A1、全部双路对抗评审修复);Phase 3 真机冒烟与 Phase 5 真机端到端
> **等开发者执行**,步骤见 docs/TODO.md §1.5。合成帧 fail-closed 全谱已进 CI;
> 真实截图回归集待真机资产产出后建立。

## Context

目前唯一能跑的是 `m0-demo`（7,753 行,真机验证过后台点击),但它绕过了整套 S0 授权设计,
阈值模型与标注系统不兼容。6,530 行标注后端 + 1,082 行 workbench 没有任何可执行文件链接,
从未在测试之外执行过。缺的三样东西都不是 GUI:manifest 读取路径、链接边、
`PixelRect`(整数)→`Detection`(浮点)的证据通道(`action_target` 根本不被评估)。

本任务:建立产品最终架构 `modules/engine`(平台无关、端口化),打通
A1+B1 真机端到端,完成标注系统的最小闭环。m0-demo 冻结保留,引擎达到真机等价后另行退役。

## 已锁定的方向(用户 2026-07-23 拍板)

1. **Engine+B1 先行**,GUI (A1) 随后。
2. **端口化**:engine 平台无关,定义 capture/input/trace 端口;controller 做 Windows 适配器;
   组合发生在 entry。Fake Controller 实现同一端口进 CI。
3. **m0-demo 冻结**:不演进、不迁移、不删;真机等价后退役(不在本任务内)。
4. **完成标准 = 真机端到端**:workbench 标注卡厄斯梦境一个页面 + 一个按钮 →
   `umbra-flow` 读生成的 manifest → 真机后台识别+授权+点击成功,全程 trace 可诊断;
   外加 Fake FrameSource + 静态截图 CI 回归。

权威约束:S0 设计 `docs/plans/2026-07-22-annotation-design.md`(锁定)、
D0–D10 `docs/plans/2026-07-21-lua-task-model-grill-decisions.md`、
roadmap `docs/plans/2026-07-21-product-form-and-roadmap.md`。

细节裁决(用户第二轮拍板):

5. **API 镜像 Luau 表面**:engine 提供 `Observation`(帧句柄)对象——
   `session.observe() -> Observation`、`observation.resolvePage()`、
   `observation.findAction(id)`;任一坐标动作后整个 Observation 失效
   (D0/D1 帧作废编进类型)。B2 时 Luau 1:1 绑定,不重构。
6. **图像资产**:CI 测试只用 KB 级合成 PNG(入仓);卡厄斯梦境真实截图回归集
   放 gitignore 目录只在本机跑,ctest 检测目录存在才注册。
7. **ImGui 已批准**:docking 分支最新 release tag,submodule 置于
   `entry/workbench/external/imgui`,D3D11+Win32 backend,方式同 Luau。

另定(可推翻):模块名 `modules/engine`;产品入口沿用 `umbra-flow` 加 `run`
子命令;目标发现留在组合根,engine 只接收已绑定目标的端口;Phase 3 冒烟任务为
硬编码 C++ 流程(不发明声明式配置,那是 Luau 的位置);trace schema 实现期定稿、
预留版本字段。

## 目标架构

```text
entry/umbra-flow (runner)   -> engine + controller + image   (组合根,Windows)
entry/workbench (GUI)       -> annotation + engine + controller (Windows)
engine                      -> core, domain, annotation        (平台无关,新模块)
annotation                  -> core, domain, vision, image     (不变)
controller (Windows)        -> core, domain                    (不变)
```

`modules/engine`(namespace `uf::engine`)承载 D10 的 Engine API 语义,内容:

- **ports.hpp** — 三个纯虚端口,只用 domain 类型:
  - `FrameSource`:`capture() -> Result<Frame>`、`validateTargetInstance() -> Status`
    (按 `WgcCaptureSession` 表面建模,适配器为薄包装)
  - `ActionSink`:`click(Point<ClientSpace>) -> Result<DeliveryReceipt>` + 投递前
    revalidate 语义(承接 `TargetMachine::revalidate` + `requireUnchangedTarget`)
  - `TraceSink`:结构化事件流(observe/resolve/authorize/act/stop),D4 要求
    错误在抛出瞬间 emit
- **runtime-loader** — 全仓第一条读取路径:项目目录 → 读
  `generated/annotations.runtime.toml`(复用 `annotation::parseRuntimeManifest`)+
  按 hash 读 `assets/templates/*.png` → `RecognitionRuntime::create`。
  fingerprint 来自 manifest 自身;`project.toml` 读取本任务不做(见 Open items)。
- **session / loop** — 镜像 D1 Model B 的 Observation 句柄 API:
  - `EngineSession::observe() -> Result<Observation>`:capture → 持有帧 + 租约;
    `Observation::resolvePage()`(有界 `evaluatePage` → `PageOutcome`)、
    `Observation::findAction(RecognizerId)`(action_target 评估,缺席=Tier A 返回空)
    ——同一帧多查询只抓一次帧
  - `EngineSession::act(...)`:`ResolvedPage` + `ActionDetection` + `ObservationLease`
    → `authorizeCoordinateAction` → 默认点击点(defaultClick 偏移,否则矩形中心,
    S0 §1.5)→ frame→client 变换 → `ActionSink::click` →
    **作废该 Observation 及其派生句柄**(D0/D1 编进类型:已失效句柄的任何取用
    → `StaleObservation` fail-closed)
  - `EngineSession::wait(...)`:有界重观察直到页面命中/超时;
    内部留 D6 弹窗清扫 hook(空实现,P0-C 填)
- **trace** — JSONL 事件词汇(page/recognizer/sadScore/maxSad/lease/stop reason),
  平台无关写 ostream;错误 kind(`AutomationErrorKind`)全程保留,
  Tier A/B/C 分类留给未来脚本边界(D4),engine 不做。

**扩展接缝**(代码内注释点名):端口→P3 第二平台/Fake CI;session 生命周期→
D10 `load_project/start_task/cancel`,P2 常驻 Engine 不改 API 表面;
wait hook→D6/P1 `bot:on`;engine 操作面(capture/find/click/wait)→B2 Luau 1:1 绑定。

## 前置(不属于本计划执行,但先于它)

- 用户提交当前工作区(33 改 + `scripts/member_init.py` **必须一起 add**);
  与 `origin/master` 分叉(落后 `eb2f5cc`)需先合并,否则推不上去。

## Phase 1 — domain/annotation 补缺口

1. `modules/domain/source/domain/space.hpp`:加 `pixelRectToFrameRect`
   (`frameRectToPixelRect` 的逆,整数 <2^24 精确,已有 `s_maxExactFrameDimension`
   注释背书)+ 精确性测试。
2. `modules/annotation`:`RecognitionRuntime` 增加 action_target 评估
   (单 recognizer、同一有界 policy、产出 `AnchorEvidence`;miss 为 Tier A 缺席不是错误)
   + 默认点击点推导(defaultClick 偏移 else 矩形中心,整数)。engine 由
   evidence.matchedRect → `pixelRectToFrameRect` → `Detection`
   (label=recognizer name, confidence=displayConfidence)→ `ActionDetection::create`。
   测试:命中/缺席/stop reason/点击点边界。
3. `modules/image/source/image/ffi/png-encoder.cpp`:**pin stb 编码配置**
   (显式设 compression_level/filter,加已知输入→已知字节的测试钉住)。
   必须在第一批真实资产生成前落地,否则日后 pin 会作废全部 template_hash。

## Phase 2 — modules/engine

4. `modules/engine/manifest.txt`:`public = core domain annotation`(无 private;
   模板 PNG 解码在 annotation 内部,loader 只读字节)。
5. `ports.hpp` / `runtime-loader.{hpp,cpp}` / `trace.{hpp,cpp}` /
   `session.{hpp,cpp}`(observe/act/wait)。
6. `tests/engine/`:Fake `FrameSource` 回放合成 PNG 帧(KB 级,入仓;即 TODO §2
   的 Fake Controller 帧序列,CI 标签)。覆盖:完整 observe→resolve→authorize→act
   happy path;fail-closed 全谱——Unknown/Ambiguous/每个 stop reason/租约过期/
   fingerprint 不符/非 ResolvedPage 动作/动作后复用失效 Observation →
   **全部零投递 + trace 记录**(S0 §6 验证门 1 第一次成为可执行测试)。

## Phase 3 — 组合根 `umbra-flow run`(Windows)

7. `entry/cli` 扩为真正产品入口:`umbra-flow run --project <dir> --selector <...>`。
   新增 runner 源:WGC 适配器(`FrameSource` over `WgcCaptureSession`)、
   输入适配器(`ActionSink` over `DeliveryTarget::click` + revalidate,
   复用 m0-demo 的 poison/补 Up 模式——**复制语义不链接其代码**,保持冻结)、
   Ctrl-C guard、JSONL trace 落盘。entry 链接 engine+controller:
   第一个同时链接两者的二进制。
8. 手写最小 manifest + 手裁模板(临时测试资产)真机 smoke:
   识别卡厄斯梦境一个页面并授权点击。等价检查点:严格后台不抢焦点、K2 delta=0、
   租约 fail-closed 行为与 m0-demo 一致。
   **假设**:本阶段 umbra-flow 整体提权运行(单进程);m0-demo 的分进程提权
   input-agent 架构留到 P0-C 硬化时再引入(见 Open items)。

## Phase 4 — A1 最小 workbench GUI

9. **[已批准]** vendor Dear ImGui docking 分支最新 release tag(submodule,
   方式同 Luau),置于 `entry/workbench/external/imgui`;
   D3D11+Win32 backend 封装在 `entry/workbench/platform/`。
10. 新可执行 `umbra-workbench`:
    - 打开/新建项目:**新增 load 路径** `loadAuthoringProject`
      (读 annotations.toml + sources,复用 `parseAuthoringDocument`——目前只写不读)
    - WGC 抓帧为 source / 导入 PNG;画布缩放平移;框选编辑 `template_rect` 与
      `search_roi`(视觉区分、包含关系可见)
    - 属性面板:名称/类型/page/整数基点阈值/click offset/required-forbidden
    - ID 铸造:`ResourceId` 增加 `fromBytes(span<byte const,16>)`,
      workbench 端注入随机字节(authoring 期允许熵;runtime 侧不变)
    - 保存走现成 `saveAndGenerateAuthoringProject`;undo/redo 接现成
      `AuthoringEditHistory`
    - 最小 Preview:对当前 source 跑 engine 同一 `RecognitionRuntime`,
      显示命中框 + sadScore/maxSad
11. load 路径 round-trip 测试(GUI 本体人工验证)。

## Phase 5 — 真机端到端 + 回归集 + 文档

12. 用 workbench 给卡厄斯梦境标一个页面 + 一个按钮 → `umbra-flow run` 吃生成
    manifest → 真机后台点击成功,trace 定位到 page/recognizer/confidence/lease。
13. 把授权的 source 截图收进正/负例回归:`runAuthoringRegressions` 接一个
    测试目标;真实截图放 gitignore 目录只在本机跑(ctest 检测目录存在才注册),
    CI 只跑合成图回归(截图属 dev/test bundle,不进 runtime 包,S0 §1.1)。
14. 文档同步:`docs/ARCHITECTURE.md`(新模块图,补 m0-demo 条目,改 capture 措辞)、
    `docs/TODO.md`(§1 勾选状态按实际改)、本计划落
    `docs/plans/2026-07-23-engine-architecture.md`。

## Verification

- 每阶段:`post-change-validation` 全门禁 + `ctest -L CI`(Windows `x64-debug`)。
- Phase 2 出口:fail-closed 全谱测试绿,即 S0 §6 门 1 可执行化。
- Phase 3 出口:真机 smoke 通过(后台、K2 delta=0、租约 fail-closed)。
- Phase 5 出口 = 任务完成标准:真机端到端 + CI 回归绿 + 文档同步。

## Out of scope(明确不做)

Luau 绑定与沙箱(B2,加固台账另行执行)、D6 清扫机制(只留 hook)、
pause/resume、分辨率自适应、OCR、A2 多页 required/forbidden 编辑 UX、
A3 批量/样本管理、托盘 App、workbench 发布回滚窗口修复(维持现文档化行为)、
m0-demo 退役。

## Open items

- **提权模型**:Phase 3 以整体提权单进程跑真机;若 UIPI 实测仍拦
  (游戏窗口完整性更高),再把 m0-demo 的 input-agent 协议语义复制进 runner
  适配层。届时是复制不是链接。
- **project.toml** 读取(项目级 fingerprint 权威)推后:P0 fingerprint 以
  runtime manifest 内嵌值为准。
- 阈值语义:engine 只认基点模型;m0-demo 的 `--threshold` 记录值随其冻结,不迁移。
