# 2026-07-21 — 脚本语言裁决:Luau

> Frozen ruling, moved verbatim out of
> [the product roadmap](../plans/2026-07-21-product-form-and-roadmap.md) on
> 2026-08-19. A roadmap is rewritten whenever status moves; this ruling is not,
> so it lives here. Its 2026-07-27 and 2026-07-29 parentheticals are part of the
> original record and are preserved as written.

## 决策

- **P0 立即采用 Luau**;没有触发重评条件时,P1–P2 继续沿用。
- 研究基线为 Luau 0.730;接入时固定精确 tag/commit,升级必须经过中断、沙箱、确定性与热加载回归测试。
- C# 保留为未来**独立 worker 路线**候选,不作为当前默认。
- D0–D10 中与语言无关的任务语义、取消分层、trace 与 generation 思路继续作为存款;
  Lua/sol2 专属实现细节不构成约束,需映射到 Luau 后重新验证。

## 为什么当前选 Luau,不选 C# worker

| 判据 | Luau(进程内) | C#(独立 worker) | 当前裁决 |
|---|---|---|---|
| C++ 易集成 | 官方 CMake targets,直接嵌入 Compiler/VM | 需 .NET/Roslyn、进程生命周期、IPC 与协议版本 | **Luau** |
| 沙箱 | VM 原生 capability 白名单、readonly 环境 | 上限更高,但必须另做 OS 低权限与资源限制 | P0 **Luau 足够** |
| 死循环硬停 | interrupt safepoint + yield/abandon | 超时可杀整个 worker | C# 上限更高,Luau 满足 P0 |
| 确定性治理 | 小语言、单线程 VM、能力面窄 | 完整 BCL/线程池/反射扩大非确定性面 | **Luau** |
| 上手 | Lua 风格短脚本 + 可选类型/静态分析 | IDE/类型系统更强,但脚本与部署更重 | 当前偏 **Luau** |
| 热加载 | 新 VM generation 验证后原子切换 | 新 worker generation 原子切换 | 平手 |
| P0 成本 | 单体 C++ 垂直切片 | 提前增加第二工具链和分布式生命周期 | **Luau** |

当前产品是个人 App、单游戏做深,P0 首要任务是让第一条真实日常尽快形成闭环。脚本由开发者自己编写,
当前威胁模型主要是误写、死循环与资源失控,不是运行未知第三方恶意代码。Luau 已提供专门面向游戏脚本的
[沙箱原语](https://luau.org/sandbox/)与
[interrupt callback](https://luau.org/api/#callbacks);[官方测试](https://github.com/luau-lang/luau/blob/0.730/tests/Conformance.test.cpp#L3370-L3489)
覆盖 interrupt 中 yield 后由宿主放弃无限循环线程的模式。

C# 的强隔离收益主要来自**进程边界**,而不是语言本身。现代 .NET 不再把 CAS/AppDomain 当安全边界,
`Thread.Abort` 也不受支持;微软对不可协作终止代码的建议是放入独立进程后杀进程
([安全边界](https://learn.microsoft.com/en-us/dotnet/core/porting/net-framework-tech-unavailable)、
[线程终止](https://learn.microsoft.com/en-us/dotnet/standard/threading/destroying-threads))。
未来若必须升级到进程隔离,同一 worker 边界也可以继续运行 Luau,无需仅为获得 `Process.Kill` 而更换脚本语言。

## Luau 落地约束

- **每任务独立 VM generation**:独立 allocator 配额、全局环境与执行线程;任务结束即可整体回收。
- **只接收源码**:由受控 Luau compiler 生成 bytecode 并立即加载;不接受磁盘、网络或用户提供的 bytecode。
- **最小 capability API**:脚本只看到 `uf.*`;不暴露文件、网络、进程、环境变量、动态库与真实系统时钟;
  宿主 API 表及其嵌套对象递归 readonly。
  *(2026-07-29 修正:根由 `umbra` 改名为 `uf`,见
  [`2026-07-29-three-layer-task-system.md`](../archive/plans/2026-07-29-three-layer-task-system.md) §6/§18。
  同文 §5/§7 进一步把 project 环境收窄为「`uf` 资源根 + `ctx`」,裸动词不在其中。)*
- **取消不可被脚本吞掉**:任务由 coroutine/`lua_resume` 驱动;其他线程只设置 atomic cancel;
  interrupt callback 检测取消后 yield,宿主不再 resume 旧线程。不得以可被 `pcall` 捕获的普通脚本错误作为最终取消信号。
- **宿主调用必须有界**:interrupt 只能抢占 Luau 执行,不能抢占卡死的 C++ binding。截图、识别、等待与输入 API
  必须支持 deadline/`stop_token`,不得无限同步阻塞。
- **确定性由宿主协议保证**:注入逻辑时钟和固定算法 RNG;禁止决策依赖 dictionary 遍历顺序;
  trace 记录 runtime/compiler 版本、脚本 hash、资源 hash、seed、observation、宿主 API 返回与 reload 事件。
- **热加载使用 generation swap**:后台编译并自检新 generation,只在任务安全点原子切换;
  不修补活跃调用栈、closure 或对象,持久状态只通过宿主定义的版本化 schema 迁移。
- **为未来 worker 留缝**:`IScriptRuntime` 边界只传可序列化 DTO;截图、识别、输入发送、按键持有账本和 trace
  归 C++ 宿主所有,禁止脚本持有 C++ 裸指针或不可序列化内部对象。
  *(2026-07-27 裁决:本条绑定的是未来跨进程 worker 接缝;P0 进程内脚本句柄用
  opaque userdata。**2026-07-29 修正**:原引用的 `docs/adr/0001-script-handles-are-userdata.md`
  已被开发者删除,该论证完整保留于
  [`2026-07-29-three-layer-task-system.md`](../archive/plans/2026-07-29-three-layer-task-system.md)
  §11「为什么句柄不是可序列化 DTO」,四条理由与本条的绑定关系见该节;
  跨进程 worker 接缝拿到的是同文 §5 的 12 个原语签名。)*

## 一票否决验证

1. 普通与嵌套 `pcall` 包裹的无限循环均能在 P0 的 500ms 总退出目标内停止,且脚本不能恢复执行。
2. 无限分配、深递归与重字符串操作只终止对应任务,不得拖垮宿主进程
   (终止机制 = allocator 硬配额 + 指令/时间预算 interrupt;LUA_ERRMEM/栈溢出
   是可被 pcall 捕获后继续的普通错误,不构成本条的停机保证)。
3. 文件、网络、进程、环境变量、动态加载与真实时钟默认均不可访问。
4. 同一 observation trace + seed 连续执行 1000 次,action trace 与最终状态 hash 完全一致。
5. 新脚本编译或自检失败时旧 generation 不受影响;成功切换后不存在新旧 closure/对象混用
   (P0 验收口径 = 加载边界:编译+自检成功才原子安装,每 generation 全新 VM;
   运行中途活体热切推 P2 常驻 Engine——2026-07-27 裁决)。
6. 人为阻塞每个长耗时 C++ binding,验证其 cooperative cancel 能满足总退出预算。

## 重新评估 C# worker 的触发条件

任一条件成立即重开选型,而不是悄悄扩大 Luau 的安全承诺:

- 开始运行下载或共享来的未知第三方脚本;
- 要求即使 VM 漏洞或宿主 binding 卡死也绝不能影响主进程;
- 脚本演化为大型业务程序,明显需要 C# IDE、泛型、LINQ、`async/await` 或 .NET 库生态;
- 产品接受 .NET/Roslyn 发布体积、冷启动、双工具链、IPC 与 worker 生命周期成本;
- P0 一票否决验证中,Luau 无法满足沙箱、不可吞取消或 500ms 停止目标。
