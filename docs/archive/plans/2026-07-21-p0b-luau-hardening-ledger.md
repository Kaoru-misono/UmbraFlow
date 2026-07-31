# P0-B Luau 宿主加固台账（实现期 checklist）

> 状态基线:2026-07-21。来源:对 Roadmap 第五节 Luau 裁决 + 6 条一票否决的**两路独立评审**
> (技术准确性核到 luau 0.730 源码 / Conformance 测试;范围与确定性缺口)。这些**不动摇 Luau 决策**,
> 全是进入 P0-B 实现时要照做的加固项。权威裁决仍见
> [`2026-07-21-lua-task-model-grill-decisions.md`](2026-07-21-lua-task-model-grill-decisions.md)(D5/D9)与
> [`2026-07-21-product-form-and-roadmap.md`](2026-07-21-product-form-and-roadmap.md) 第五节。

## 硬红线（唯一一条，做错则取消语义整体失效）

- [ ] **取消实现成 yield-and-abandon,绝不用 `luaL_error`**。Luau 0.730 `Conformance.test.cpp`
      的 `Interrupt` 用例证明:error 版取消对 `pcall(function() while true do end end)`(`hangpcall`)
      **返回 `LUA_OK` = 被吞掉**;而 interrupt 里 `lua_yield` 打断死循环、宿主丢弃该线程不再 resume,
      yield 不是 Lua error、Luau 的 pcall 可穿透 yield,故 pcall 吞不掉。D4 Tier C 已规定正确一侧,
      风险纯在实现纪律。前提:任务必须跑在 coroutine / `lua_resume` 上(主 state 上 `lua_yield` 会抛)。
      **✅ 2026-07-21 spike 实测确认(MSVC/0.730)**:具体原语用 **`lua_break`**(设 `L->status=LUA_BREAK`、
      不走 `luaD_throw`,比 yield 更干净)——对 `pcall(function() while true do end end)` 500ms 硬停、
      pcall 抓不到;`luaL_error` 版同一循环被 pcall 吞(返回 LUA_OK)。见
      [`2026-07-21-luau-integration-plan.md`](2026-07-21-luau-integration-plan.md) §1。

## 沙箱

- [ ] **宿主 `uf.*` API 表逐层递归 freeze**(2026-07-29:根由 `umbra` 改名为 `uf`,见
      [`2026-07-29-three-layer-task-system.md`](2026-07-29-three-layer-task-system.md) §6/§18;
      同文 §7 把「构造时冻结 + `__metatable` + `__index` 必须是表」定为运行期规则,
      本条的 boot 期递归 freeze 是其下限)。`luaL_sandbox` 只冻内置库/内置 metatable/全局表;
      "递归 readonly" 要宿主自己遍历每张嵌套表 `table.freeze`/设只读。漏一张嵌套表 = 一个 monkey-patch /
      跨运行泄漏洞。加一条显式沙箱测试覆盖嵌套表不可写。
- [ ] **显式 nil 掉 `luaL_sandbox` 不移除的 5 个全局**(2026-07-21 spike 实测:0.730 上它们仍存活):
      `getfenv`、`setfenv`、`newproxy`、`coroutine`、`debug`。尤其 **coroutine**(脚本自开协程逃出宿主
      interrupt 取消,D5 依赖)与 **debug**(卸钩子/越界,D9)。`io`/`package`/`require`/`load*`/`dofile`/
      `loadfile`/`collectgarbage` 与 `os.execute/getenv/remove/exit` 已被 `luaL_sandbox` 移除、`os.time` 保留,
      无需再处理。见 [`2026-07-21-luau-integration-plan.md`](2026-07-21-luau-integration-plan.md) §1/§4。
- [x] 只收源码、隔离脚本全局、移除 bytecode 摄取(`string.dump`/load-bytecode)——
      已在裁决内,实现时确认(spike 已确认 sandbox 下无 `load`/`loadstring`,脚本侧无法喂 bytecode)。
      (**2026-07-29 修正**:本条原写「`luaL_sandboxthread` 隔离脚本全局」,那个机制已被否决——
      Luau 的环境隔离按闭包、不按线程,新线程的 `gt` 是从父线程复制的,所以那个代理形状正是
      `_G` 逃逸的形状。落地形态是两张显式 env 表:framework 环境,以及**不带任何 metatable、
      因而没有 `__index` 链**的 project 白名单环境。见
      [`2026-07-29-three-layer-task-system.md`](2026-07-29-three-layer-task-system.md) §7 与
      `modules/script/source/script/ffi/environment.{hpp,cpp}`(阶段 2b-1,`67e7e63`)。
      按原文写法这个复选框永远勾不上,故就地改述后勾选。)
- [ ] 记账 allocator 必装:Luau **默认无内存上限**,靠宿主装记账 allocator 才能兑现"无限分配只终止该任务"。

## 硬取消 / interrupt

- [ ] **取消验证套件加"不可 yield 上下文"用例**:`table.sort` 比较器 / `string.gsub` 回调里写死循环——
      interrupt 触发但 `lua_yield` 在此会**抛错**(又落回可被 pcall catch 的那类)。"任何纯 Luau 死循环
      500ms 停"有这一窄例外类,可信作者模型下边角但要显式记录 + 测。
- [ ] **interrupt 守 GC 上下文**:interrupt 在 GC(`gc >= 0`)时也会被调,此处 yield/error 不安全;
      Conformance 回调用 `if (gc >= 0) return;` 守。无条件 yield 会 UB。
- [ ] **500ms 预算端到端测**:含 binding drain + 补 Up 释放按键,不只是 VM 停。

## 确定性

- [ ] **迭代顺序禁令从"决策"扩到"trace / state 序列化"**:脚本若把 `pairs` 结果写进 state hash 或 trace,
      即使没有决策依赖它也会破一票否决 #4。禁止**发出**无序迭代结果,不只是禁止据其分支。
- [ ] **加"识别器确定性"到一票否决**:同帧 → 位级相同 detection 输出。缓解事实:移植的 SAD 是整数灰度核,
      大概率天生确定;但"每次无人值守动作留可复现证据"要求识别侧也确定,须显式断言(用 Fake Controller +
      静态截图回归当断言点),不能只保脚本→动作侧。
- [ ] **一票否决 #4 永远保持"同机 1000×",不升级成跨平台位级**:超越函数(`sin/cos/exp/pow`)与任何走
      libc `snprintf` 的格式化路径跨 CPU/libm 非位级一致;D9 已把"超越函数跨平台位级复现"留作开放项,别把它
      变成已发布保证(会崩)。附:Luau 的 `tostring` 用 Schubfach(确定、不走 libc `%g`),常见浮点转字符串
      路径反而跨平台确定,比原生 Lua 好。

## 措辞修正

- [ ] **一票否决 #2 理由改写**:终止靠 **allocator 硬配额 + 指令/时间预算 interrupt**,不是 OOM 错误本身——
      `LUA_ERRMEM`(OOM)和栈溢出是**可 catch 的普通错误**,脚本能 `pcall` 后继续。别让 #2 暗示 OOM 错误本身
      就是停机点。

## 升级纪律

- [ ] **pin 精确 tag(0.730)+ 每次 Luau 升级重跑 interrupt / 沙箱 / 确定性回归**。yield-from-interrupt
      这个能力活在 VM 内部实现(`VM_INTERRUPT`),不是有稳定性保证的公开 API;官方文档措辞甚至读着像
      "interrupt 不能 yield",早期 maintainer 评论亦然——但 0.730 的 VM 和它自己的 Conformance 测试确实实现并
      覆盖了。所以"pin tag + 升级回归"是**承重纪律,不是仪式**。
