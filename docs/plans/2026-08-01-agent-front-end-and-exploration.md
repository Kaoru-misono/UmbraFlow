# Agent 前端与探索环境 — 工单 4b 形状

> 状态:**已授权执行,2026-08-01 开发者「全都动,搞干净点」**。上位裁决全部来自
> [三层与 Agent](2026-08-01-three-layers-and-agent-operator.md) §一–§三、§七 与
> [script-owned](2026-07-31-script-owned-page-model.md) §九/§十二;本文只钉实现
> 形状,不重开任何已裁问题。前置:工单 4a(旧动词退役)落地。

## 一、要建的东西

**探索环境**:第二个 Luau 环境(按 7-29 §七 的闭包隔离),动词 = 运行环境全集 +
特权动词。特权动词只在这里装载:

- `cycle_click_point`(已存在,今起只有探索环境的表上有它)
- `cycle_crop(ticket, rect) -> png blob`(新原语:区域像素出坑,Agent 的眼睛;
  blob 经 `project_write` 落进项目目录给 Agent 看)
- `probe(blob, rect, key?, tolerance?) -> stats`(新原语:色键/掩码统计,即 v4
  `frames probe` 的 l2 化;像素事实归第一层,判断归上层)
- 帧差异查询:暂缓,先用两次 crop + Agent 目视顶;有实例再立原语。

**探索通道**:`umbra-flow explore --project DIR --queue PATH --results PATH`
——Agent 逐条投 Luau 片段,宿主在探索环境执行,每条一行结果。这是 2026-08-01 §三
「逐条调动词」的正式载体,取代 drive 的模型动词与 input-agent 的裸操作:同一张
票据协议、同一个 trace 流。

**trace 归属**:探索运行以 `FrontEnd::Annotation` 记账(已有枚举值);裸点击与
crop 有自己的事件形状(`annotation.click_delivered` / `annotation.region_saved`
一类),**不得**写成 `engine.action_delivered`——裸点击没有 recognizer 没有页面,
词汇必须诚实。

**l2 原生标注回路**(可信框架例程,Agent 经探索环境驱动):
框选 → `cycle_crop` 量像素 → `probe` 定键与阈值 → 模板落 `assets/templates/` →
`model.Element/appearance` 构造 → `project.save_project` → `oracle` 记屏与期望 →
`regress` 全矩阵。教学消费协议(2026-08-01 §四)自此有了全部动词。

## 二、随后的删除清单(§九 收尾)

> Executed 2026-08-01 in `a80ea07`, recorded 2026-08-11. Every item below
> landed; read the list as a record, not as outstanding work. Item 4 in
> particular is done — `umbraflow-authoring/v4` and `umbraflow-annotations/v3`
> have no read or write path because neither id exists anywhere in `schema/`,
> `modules/`, `entry/`, `tools/` or `tests/`, and the constants that carried
> them (`k_authoringDocumentSchema`, `k_runtimeManifestSchema`) went with the
> C++ model. The RuntimeModel is `umbraflow-runtime/v2` now; see the
> **Schema ids** section of `CONTEXT.md`.

新回路对 chaos-v14 完成一次「重标一个元素 + 全矩阵绿」验收后,按序删除:

1. `entry/authoring` 的 page/element 绘制动词与 `check`(v4 版)
2. `entry/workbench` 剩余标注后端、authoring-compiler、authoring-document
3. `modules/annotation` 模型层与 recognition 栈(PageResolver 等;SAD 已在
   vision,4a 已迁模板胶水)
4. `umbraflow-authoring/v4` 与 `umbraflow-annotations/v3` 两个 schema 的读写路径
5. `entry/input-agent` 与 `entry/m0-demo`:探索通道覆盖其全部职能后一并退役
   (m0-demo 的冻结条款「真机对等后退役」由第一条真边 + 标注回路验收兑现)
6. 依赖图定格为 script-owned §三 的形状;ARCHITECTURE 重画

## 三、验收线

- 探索通道真机冒烟:队列投「crop 主菜单一角」+「裸点唤醒」+「读一格文字」,
  三行结果 + trace 全程 `FrontEnd::Annotation`。
- 标注回路真机验收:Agent 只经探索通道给 chaos-v14 新增一个元素(建议:出擊页
  的「進入」按钮),矩阵全绿,`walk_edge` 用它走一条新边。
- 删除波次后:全门禁绿;`git grep` 零残留(AnnotationType、AuthoringDocument、
  cycle_page 等旧名);ARCHITECTURE/CONTEXT/TODO 同步。

## 四、开放项(实现中裁,不阻塞)

- probe 的统计面(沿用 v4 probe 的字段起步,fully_selected_pixels 等)
- 探索队列的错误行格式(沿用 drive 的 JSON 行惯例)
- input-agent 退役前,把 capture-flanked 唤醒的经验规则移植进探索通道并复验

## 2026-08-03 — Why a session asks two latches

The VM interrupt's three triggers reach no host call, so a break by the wall
clock or the instruction budget latches inside `script::Engine` and nowhere the
task layer can see. An exploration session that consulted only its own latch
went on accepting chunks and refusing every one of them, and each refusal
refreshed the idle clock, so the abandoned session never ended on its own.
`ExplorationSession::terminalKind` therefore asks the context latch and
`script::Engine::generationSpent()` in turn.
