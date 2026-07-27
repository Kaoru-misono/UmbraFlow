# P0-B 脚本层 — 动工前裁决与执行切片

> 状态:**已裁决,2026-07-27 grill 完成**。本文补全
> [`2026-07-21-lua-task-model-grill-decisions.md`](2026-07-21-lua-task-model-grill-decisions.md)
> 中 D1/D4/D10 的「留待」项,并给出 P0-B 的执行切片。上游权威不变:产品方向看
> [roadmap](2026-07-21-product-form-and-roadmap.md),S0 契约看
> [annotation-design](2026-07-22-annotation-design.md),实现配方看
> [integration-plan](2026-07-21-luau-integration-plan.md) 与
> [hardening-ledger](2026-07-21-p0b-luau-hardening-ledger.md)。
>
> 指导原则(开发者 2026-07-27 明确):**宿主架构、模块边界、schema 与脚本可见
> API 必须从第一天就服务最终形态(P2 托盘常驻 App、P3 第二游戏),不做
> 「先随意跑通再重构」的版本。**「P0 允许笨但能跑通」只适用于 Luau 任务脚本的
> 内容(复制粘贴写每日,P1 按 D7 重构),不适用于宿主侧。

## 一、裁决汇总

1. **capability 命名空间 = `umbra.*`**。脚本全局根对象唯一,进 AST 校验器、
   trace、错误消息与全部示例;C++ 命名空间保持 `uf::` 不受影响。旧文档中的
   `bot.*` 均已加注读作 `umbra.*`(annotation-design §4 已改写)。词条见根目录
   `CONTEXT.md`。
2. **P0 脚本句柄 = 进程内 opaque userdata**([ADR 0001](../adr/0001-script-handles-are-userdata.md))。
   `frame`/`page`/`detection` 是宿主拥有、脚本不可窥视的 userdata,一比一包装
   engine 冻结句柄;「`IScriptRuntime` 只传可序列化 DTO」改判为未来跨进程
   worker 接缝的要求(触发 C# worker 重评估条件时兑现)。
3. **绑定层 = 新模块 `modules/task`**,依赖 `script + engine + annotation`。
   `modules/script` 保持纯 Luau 底座(沙箱/取消/配额住这里,不沾 engine);
   `modules/task` 承载 host-api 绑定、AST 字面量校验器、Tier 错误映射、任务
   运行循环,并且是 D10 Engine API 语义(`load_project/start_task/cancel/...`)
   的长期归宿;entry 组合根只做装配。engine 永不依赖 script/task。
4. **动作动词:click 为 P0-B 核心,keyPress 为 P0-B 内次优先扩展,拖拽/滑动
   明确推迟**(开发者确认每日大部分为点击,部分可按键,拖动暂不考虑)。
   controller 层按键机械(keyPress/keyDown/keyUp/longPress + 按住账本)已存在,
   增量在 engine `ActionSink` 端口与授权语义。**硬约束:端口与授权模型按
   action kind 参数化塑形,新增动词是加法,不改造既有链路。**
   非坐标动作(按键)要求的授权证据模型是 key 扩展动工前的前置设计项——
   底线:仍需页面证据 + fingerprint/generation 校验 + 租约,只是不绑定像素。
   若 P0-C 逐页分解发现全部按键步骤有点击等价物,key 扩展可后滑、不阻塞 B3。
5. **generation 热加载 P0 只做加载边界语义**:新 generation 编译+自检成功才
   原子安装,失败不影响已装载 generation;每 generation 全新 VM,新旧 closure
   混用在构造上不可能。一票否决 #5 按此口径验收(roadmap 已加注)。运行中途
   活体热切推 P2——generation 机械(校验→原子安装)作为独立接缝住在
   `modules/task`,P2 只是增加「任务安全点触发同一套安装」。
6. **错误暴露 = `umbra:try(fn)` 受控捕获入口 + 保留原生 `pcall`**。`try` 只捕
   Tier B 自动化结构化错误,脚本自身 bug 穿透;`pcall` 不删(语言习惯,且
   Tier C 经 `lua_break` 对其免疫)。`wait` 超时归 Tier B 独立 kind,与 Tier C
   的预算耗尽分开,不合并。
7. **任务归属项目、名字寻址**([ADR 0002](../adr/0002-tasks-are-project-owned.md)):
   任务住在项目 `tasks/` 目录,`umbra-flow run --project <dir> --task <name>`,
   宿主载入时算脚本内容 hash、连同 compiler 版本写入 trace。CLI 永不执行游离
   路径脚本。P1 D7 的任务清单与 P2 App 的任务枚举都是这一结构的加法。

## 二、被「最终形态」原则升级为 P0-B 承诺的项

- **`FrameSource::capture()` 端口补 deadline/stop_token**。「宿主调用必须有界」
  是终态契约(integration-plan §5 Risk #1);不补则 500ms 硬取消永远有一个
  不可抢占的 capture 缺口,且拖得越晚返工面越大。
- **脚本级 trace 从第一天就是正式版本化 schema**(`task-trace/v1`:seed、脚本
  hash、compiler 版本、host-API 返回、generation 事件),不往 `engine-trace/v1`
  塞临时字段;两条 trace 流并存,各自 golden 测试。
- **每任务全新 VM 在阶段 1 就纠正**。当前 `modules/script` 的持久 Engine 复用
  主 `lua_State`(globals 跨 `runNumber` 泄漏)与 D9「每 run 全新 lua_State」、
  D10「每任务全新 VM generation」相悖,属阶段 1 返工项,不留到以后。
- **单一取消源**:Ctrl-C(及未来 `Engine.cancel`)驱动同一 `std::stop_source`;
  经 `std::stop_callback` 桥接置 Luau interrupt 读的原子标志,engine 侧继续
  消费同一 token——不做两套取消。

## 三、执行切片

### 阶段 1 — 沙箱 + 硬取消 + veto 套件(integration-plan §6 step 3+4,同批 TDD)

全部在 `modules/script` 内,无未决依赖,可立刻动工:

- `sandbox.*`:建序 openlibs → 注册+递归 deepFreeze 宿主表 → nil 掉
  `getfenv/setfenv/newproxy/coroutine/debug` → `luaL_sandbox` → 每任务
  `lua_newthread` + `luaL_sandboxthread`;os.time 处理按 D9(宿主替代)。
- `cancellation.*`:atomic cancel 标志 + interrupt 回调(先 `if (gc >= 0) return;`)
  + `lua_break` yield-and-abandon;绝不 `luaL_error`(硬红线,spike 已实测)。
- 记账 allocator + 每任务内存硬配额;指令预算 + `max_runtime` 时间预算
  (常数先保守占位,首个真实任务标定)。
- per-task VM 生命周期纠正(见 §二)。
- veto 套件进 CI:6 条一票否决 + 不可 yield C 边界用例(`table.sort` 比较器、
  `string.gsub` 回调、宿主 C 函数内 hang)+ 嵌套宿主表不可写 + 危险全局缺席
  断言。veto 测试与其守护的实现同批提交,Luau 升级纪律自此可执行。

### 阶段 2 — `modules/task` 绑定层(step 5)

- 新模块 `modules/task`(manifest:public = core domain annotation engine script)。
- runtime manifest → `umbra.recognizers/pages` 递归只读句柄表;Luau AST 校验器
  在 VM 创建前 100% 枚举字面量引用(annotation-design §4)。
- userdata 包装:`umbra:capture()`/`frame:resolve_page()`/`frame:find()`/
  `umbra:click()`/`umbra:wait_for_page()` 一比一映射 EngineSession 语义,
  act 后作废、四要件授权、fail-closed 全部保全;`__metatable` 保护。
- Tier A/B/C 映射:空 optional→`nil`;StaleObservation/Timeout/ActionRejected/
  RecognitionFailed→可捕获结构化错误(retryable 默认值在此定表);
  Cancelled/预算耗尽→特判走 Tier C 不可吞路径。`umbra:try` 落地。
- 任务 sourcing:`tasks/` 目录 + 名字寻址 + hash 入 trace(ADR 0002);
  `umbra-flow run` 改为 `--project --task` 形态。
- API 草图先行:动工前出一版 `umbra.*` 表面签名清单(含 raw find 暴露与命名
  细节)供开发者过目,再写绑定。

### 阶段 3 — B2 收尾

- `umbra:now()` 逻辑时钟 / `umbra:random()` 固定算法 RNG(seed 入 trace);
  有序遍历纪律(禁把无序迭代结果发出到 trace/state)。
- `task-trace/v1` schema + golden 测试;确定性回归 harness(可复用
  Fake Controller + 静态截图 golden——目前不存在,新建)。
- veto #4 跑通:同 observation trace + seed 1000× 同机复现。
- `sweepKnownPopups` 从 no-op 变最小 D6 清扫(观察周期边界 + 长 wait 内)。
- key 动作扩展(若 P0-C 分解确认需要):ActionSink 新 kind + 非坐标授权证据
  设计 + 授权链/trace 词汇扩展。
- `capture()` deadline/stop_token 端口补齐(可与阶段 1/2 并行,谁先方便谁做)。

之后进入 B3/P0-C(整套每日、遮挡/最小化/CaptureStalled、长程),不在本文范围。

## 四、遗留待定(记账,不阻塞动工)

- 常数标定:interrupt 回调频率/指令预算、`max_runtime` 默认、allocator 配额、
  `max_action_frame_age` 生产值——保守占位,首个真实任务 + 真机 soak 标定。
- D9 确定性地板逐项(`setmetatable`/`raw*`/`next`/`tonumber`、integer/float
  边界、地址型 `tostring` 消毒):阶段 1 按「能禁则禁」的保守默认落,逐项在
  hardening-ledger 记录。
- OCR/`info_region`:P0-C 分解时按 annotation-design §7 契约裁决,默认全
  `gray_template`。
- 卡厄斯梦境每日流程分解、接管起点、运行分辨率/DPI(roadmap §四待开发者输入)。
- workbench「共享 action target」在 Save+Generate 时展开为每页 recognizer
  (形如 `back_<page>`),对 `umbra.recognizers.<name>` 脚本表面的可读性影响
  ——2026-07-25 UI 重设计时已标注「P0-B 落地时复核」,归阶段 2 API 草图评审。
