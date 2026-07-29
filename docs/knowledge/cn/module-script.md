# `modules/script` 架构知识

> **DIRTY（2026-07-29）**：本文写作时 `modules/script` 还只是一层 VM 包装。此后该模块
> 陆续拿到沙箱、记账 allocator、interrupt 取消，以及（`67e7e63` / `e89bc53`）双环境
> 拆分（`environment.{hpp,cpp}`）。下文中已明确错误的论断已就地更正，但逐步数据流叙述
> 与测试清单尚未重新推导。以实际代码与
> `docs/plans/2026-07-29-three-layer-task-system.md` 为准，待重新同步。

`modules/script` 是 UmbraFlow 的 Luau 底座。C++23 宿主运行固定在 0.730 的 Luau，外面包着
沙箱、记账 allocator 和 interrupt 驱动的取消，并且每个 VM 都以**两个环境**启动：一个受信
framework 环境，和一个没有任何 `__index` 链能到达它的 project 环境。该模块仍然不拥有的是
task policy——等待、重试、step、interrupt——那属于 `modules/task/runtime/` 下的 Luau
framework。

## 当前能力与限制

模块当前拥有以下职责：

- 以 `uf::script::Engine` 作为唯一公开类型，管理一个嵌入式 Luau `lua_State` 的完整生命周期。声明位于
  `modules/script/source/script/engine.hpp`，实现位于 `modules/script/source/script/ffi/engine.cpp`。
- 接收 Luau 源码和 chunk 名称，以 compiler 生成 bytecode，加载到新 coroutine 后同步执行。
- 把 VM 创建失败、编译/加载失败和脚本运行失败转换为仓库统一的
  `Result<T>`/`Error` 通道，而不是把 Luau 状态码或 C API 类型暴露给调用方。
- 把第三方 C API、C 分配内存和 `lua_State*` 限制在 `modules/script/source/script/ffi/`
  边界内；公开头文件不包含任何 Luau 头文件。
- 在 `modules/script/external/` 下封装 vendored Luau 构建；`modules/script/external/CMakeLists.txt`
  只启用宿主需要的库。

它不负责以下工作：

- 不拥有 observation、page resolution、recognition、action、trace 或 lease；这些能力位于
  `modules/annotation`、`modules/engine`、`modules/controller` 等模块，`script` 当前没有连接它们。
- 不拥有 Windows capture 或 input。`modules/script/manifest.txt` 没有
  `platforms = windows`，因此它本身是跨平台模块；严格后台投递仍是
  `controller` 和组合根的责任。
- 不提供任务文件加载、`require`、多文件子任务、任务队列、热加载、持久状态或 host binding；公开面只接受
  调用方已经持有的 `std::string_view` 源码。
- 不做 Luau offline analysis、JIT、CLI、Web 或 Luau 自带测试。构建只链接
  `Luau.VM` 与 `Luau.Compiler`；`Luau.CodeGen`、`Luau.Analysis`、
  `Luau.Config`、`Luau.Require` 均不在模块依赖中。
- 不拥有 task policy。等待、重试、step 嵌套和 interrupt 注册表住在
  `modules/task/runtime/` 下的受信 Luau framework 里，不在这里。`script` 提供那些策略
  运行其上的底座：环境、冻结、预算，以及宿主装自己表的接缝。

本文初稿写作时没有、而今天**已经拥有**的，是整套安全底座：

- `installSandbox`（`modules/script/source/script/ffi/sandbox.cpp`）剥掉 `luaL_sandbox`
  留下的危险全局（`getfenv`、`setfenv`、`newproxy`、`gcinfo`、`coroutine`、`debug` 和
  `_G`）以及残余的时钟与 RNG 入口（`os.time`、`os.clock`、`os.date`、`math.random`、
  `math.randomseed`），然后执行 `luaL_sandbox()`。
- `createStateWithQuota`（`ffi/allocator.hpp`）安装记账 allocator，强制每任务内存硬上限；
  超配额的增长表现为可捕获的 `LUA_ERRMEM`，而不是把宿主一起拖垮。
- `ffi/cancellation.cpp` 装一个由 stop token、指令预算和 `maxRuntime` deadline 驱动的
  interrupt callback。它的 GC 上下文守卫（`if (gc >= 0) return;`）是 callback 的第一条
  语句，符合 hardening ledger 的硬红线。
- `ffi/environment.{hpp,cpp}` 构造下面说的两个环境。

`docs/plans/2026-07-21-p0b-luau-hardening-ledger.md` 与
`docs/plans/2026-07-21-luau-integration-plan.md` 是这批工作的历史记录。两者都早于双环境
设计，且仍在教用 `luaL_sandboxthread` 隔离脚本全局；那个机制为何被否决，见下文「两个
环境」。

模块已经在产品链接闭包内。`modules/task/manifest.txt` 把 `script` 声明为 public 依赖，
`entry/CMakeLists.txt` 又把 `${PROJECT_NAME}_task` 链进 `umbra-flow` 的 CLI support
library，所以底座经由 `task` 进入发布可执行文件。`script/engine.hpp` 的入站 include 来自
`modules/task`（`capability-surface.hpp`、`framework-bundle.{hpp,cpp}`、`task-host.cpp`）
以及 `tests/script/` 与 `tests/task/`。

### 两个环境

Luau 的环境隔离是**按闭包，不按线程**：`luau_load` 收的是 chunk 闭包携带的 env 表，而新
线程的 globals 表是从父线程复制的。所以 `luaL_sandboxthread` 那套代理形状**不能**在同一
个 VM 上分开两个信任层级——那个代理形状正是设计要排除的 `_G` 逃逸形状。它在这里是**已被
否决**的机制，不是待办项。两张显式 env 表可以做到它做不到的事：

- **framework 环境**是一张可写的表，其冻结的 metatable 把 `__index` 链到主 globals。受信
  framework 模块在它下面加载，自己的全局不落到主表上。
- **project 环境**是显式白名单，**完全没有 metatable**。这个「没有」就是隔离性质本身：
  没有 `__index` 就没有通往 framework 环境或主 globals 的链，于是否定名单由结构成立，而
  不是靠逐条枚举。白名单里点名却在来源表中缺失的名字会让整个 generation 失败，而不是悄悄
  产生一个更薄的环境。

两张表都只住在 VM registry 里，因此从任何全局表都不可达。project 环境注册的是一张**冻结
的原型**，`pushProjectEnvironment` 给每次 run 一份新的可写浅拷贝——一次 run 写下的全局随
那份拷贝一起死，而它与原型共享的值仍然是冻结的。

## 执行流程

### 公开表面

`modules/script/source/script/engine.hpp` 的公开表面是 `uf::script::Engine`，以及它需要的
配置与冻结词汇：

- `Engine::create(EngineConfig const& = {}) -> Result<Engine>` 是命名工厂；VM 分配可能失败，
  所以没有 public constructor。`EngineConfig` 携带 stop token、内存与指令预算、
  `maxRuntime`、`frameworkModules` bundle、两个 installer 接缝（`HostTableInstaller` 与
  `PrivateCapabilityInstaller`，都返回 `Status`），以及两张白名单 `projectGlobals` 与
  `frameworkProjectGlobals`。
- `deepFreeze(lua_State*, int index) -> Status` 递归把一张表、从它的值可达的一切，以及沿途
  每张 metatable 标为只读，顺序是先 metatable 后表。它同时强制 project 可见宿主对象必须
  满足的两条结构规则：每张 metatable 都带 `__metatable` 字段（没有它，`table.clone` 会返回
  **可变副本、带同一个 metatable**，于是「用 metatable 证明身份」可被伪造），且 `__index`
  一律是表、绝不是函数。
- `deepFreezeMetatable(lua_State*, int index) -> Status` 把一张表**当作 metatable** 检查并
  冻结，用于「metatable 先构造并注册、之后才挂到对象上」这个常见情形。
- `Engine::runNumber(std::string_view source, std::string_view chunkName)
  -> Result<double>` 是唯一执行入口。
- `Engine(Engine&&)`、move assignment 和析构函数公开；`std::unique_ptr<Impl>` 使 copy 不可用。
- `Engine::Impl` 与接收 `std::unique_ptr<Impl>` 的 constructor 都是 private，不能构造无有效 VM 的
  `Engine`。

`Result<T>` 的真实定义在 `modules/core/source/core/error/result.hpp`，是
`std::expected<T, Error>`。实现使用的 `AutomationErrorKind`、`fail(...)` 和
`automationErrorKind(...)` 位于 `modules/domain/source/domain/error.hpp`。
`modules/script/manifest.txt` 实际把 `core domain` 都声明为 public；当前实现用 domain 分类错误，测试再按
该分类检查失败。

### 创建路径

`Engine::create()` 的同步数据流是：

1. `luaL_newstate()` 创建主 `lua_State`。
2. 返回 null 时返回 `AutomationErrorKind::InternalInvariant` 和消息
   `luaL_newstate returned null`；不会产生半初始化 `Engine`。
3. 成功后，`luaL_openlibs(state)` 打开完整标准库。
4. `state` 被交给 `std::make_unique<Engine::Impl>`。
5. private constructor 把唯一所有权移入 `Engine::m_impl`。

`Engine::Impl` 在 `modules/script/source/script/ffi/engine.cpp` 中定义。它保存 `lua_State* m_state`，
删除 copy/move，并在析构时对非 null state 调用 `lua_close()`；raw pointer 不越过 FFI 实现文件。

这一层 pImpl 有两个目的。第一，公开头文件只依赖 `core`、`<memory>` 和
`<string_view>`，Luau ABI 不成为 `script` 的公开 ABI。第二，C API 的所有权
证明和编译器 warning suppression 都集中在一个可审计文件中。

### 单次执行路径

`Engine::runNumber()` 的数据流按代码顺序如下：

1. 从 `m_impl->m_state` 取得主 state。
2. 构造 `lua_CompileOptions`，显式固定 `optimizationLevel = 1` 与
   `debugLevel = 1`。
3. 调用 `luau_compile(source.data(), source.size(), &options,
   &bytecodeSize)`。源码通过 pointer + size 传入，不要求 null 结尾。
4. `luau_compile()` 返回的 `char*` 为调用方所有；null 被映射为
   `AutomationErrorKind::InternalInvariant`。
5. `scopeExit(...)` 保证所有返回路径调用 `std::free()`；其实现位于
   `modules/core/source/core/utility/scope-exit.hpp`。
6. 用 `lua_gettop(state)` 记录 `stackBase`，再由 `lua_newthread(state)` 创建并压栈 coroutine，使其可达。
7. 第二个 `scopeExit(...)` 总会执行 `lua_settop(state, stackBase)`，弹出 coroutine 并恢复主栈。
8. `chunkName` 被复制成 `std::string`，为 `luau_load()` 提供稳定的 null-terminated 名称。
9. `luau_load(thread, name.c_str(), bytecode, bytecodeSize, 0)` 加载 bytecode；非 `LUA_OK` 返回
   `AutomationErrorKind::InvalidResource`。
10. `lua_resume(thread, nullptr, 0)` 同步运行脚本。`LUA_YIELD` 被明确拒绝，
    因为当前宿主没有 resume protocol；其他非 `LUA_OK` 状态也返回
    `AutomationErrorKind::InvalidResource`。
11. 成功后，栈非空就以 `lua_tonumber(..., -1)` 转换最后一个返回值；栈为空返回 `0.0`。

错误文本由文件内 helper `topError(lua_State*)` 读取栈顶。若
`lua_tostring(..., -1)` 返回 null，它使用固定文本
`(non-string error value)`，避免错误报告本身依赖一个必然为字符串的假设。

“每次新 coroutine”不等于“每次新 VM”：同一个 `Engine` 的多次 `runNumber()` 共享主
`lua_State`。但全局已经不再跨 run 泄漏。`runNumber` 走
`runNumberInProjectEnvironment`，它为本次调用从冻结原型建一份新的 project 环境、用完丢弃，
并在 `luau_load` 之前用 `lua_setfenv` 把它绑到本次 run 的线程上。绑线程正是沙箱需要的
C++ 侧 setfenv 路径——Lua 自己的 `setfenv` 已被移除——而且它有两重意义：不绑的话，该线程上
的 `LUA_GLOBALSINDEX` 会够到主 globals；而且 `luau_load` 会针对已冻结（因而被标记
`safeenv`）的主 globals 预解析 import 常量。

## 必须保持的约束

### 当前已经成立的 fail-closed 行为

对现有窄 API，失败不会伪装成有效数字：

- VM 或 compiler buffer 分配失败返回 `InternalInvariant`。
- 语法错误由 Luau 编码进 error bytecode，并在 `luau_load()` 阶段变成
  `InvalidResource`。
- load error、runtime error 和未被宿主支持的 yield 都返回
  `InvalidResource`。
- 只有 `lua_resume()` 返回 `LUA_OK` 才读取结果。

这里的 fail-closed 只覆盖“能否完成一次 `runNumber()`”。它不等价于产品的
动作安全门，因为当前 API 根本没有 capture、recognition 或 input capability。

### 所有权与生命周期

- `Engine` 通过 `std::unique_ptr<Impl>` 唯一拥有 VM；move 转移所有权，
  copy 在类型层不可表达。
- `Impl::~Impl()` 与 `lua_close()` 配对，保证正常离开作用域时回收整个 VM。
- `luau_compile()` 的 C allocation 由第一个 scope guard 配对
  `std::free()`；`luau_load()` 后无论成功、失败或后续字符串分配抛异常，
  buffer 都会释放。
- coroutine 由主 state 栈临时保持可达；第二个 scope guard 恢复原始栈顶，
  因而重复调用不会把 coroutine 永久留在主栈。
- `source` 和 `chunkName` 只在本次调用期间有效，不会存入 `Engine`。
  `chunkName` 在需要 C string 时先复制；源码只在同步 compile 调用期间借用。

这些机制说明为什么危险操作位于
`modules/script/source/script/ffi/engine.cpp`：`std::free()` 和外部 handle
管理需要局部的 `// SAFETY:` 证明，而边界外只看到 RAII value 与
`Result<T>`。

### 线程约束

`Engine` 的头文件明确声明 NOT thread-safe：所有 VM 调用必须发生在 owning thread。现有实现没有 mutex；
计划中的 watchdog 只能设置 atomic flag，不能从其他线程调用 Luau API，interrupt callback 尚未落地。

因此，当前可验证的不变量是 thread confinement，不是 thread safety。调用方
若并发调用同一 `Engine`，已超出契约。

### 确定性现状

compiler 的 optimization/debug level 已固定，确定性下限现在是被强制的，而不是计划中的：

- 残余的真实时间与随机入口已被移除——`os.time`、`os.clock`、`os.date`、`math.random`、
  `math.randomseed`——所以脚本唯一的随机来自宿主的 seeded RNG，而且完全读不到任何时钟；
- globals 不再跨 `runNumber()` 泄漏：每次 run 拿到一份新的 project 环境；
- host-controlled seeded RNG 在 `modules/task` 里，seed 每 run 注入并写进 `run.started`。
  按 `docs/plans/2026-07-29-three-layer-task-system.md` §10，逻辑时钟是要**删掉**而不是保留
  的——阶段 3 由基于证据的等待（`ctx:wait_for_page`）与声明式停顿（`ctx:settle`）取代它。
- 仍然没有机制限制 dictionary 迭代结果进入决策或序列化；那一条目前只是约定。

所以“同一 observation trace + seed 运行 1000 次结果完全一致”仍是 veto gate 而非已兑现的
保证，但底座本身已经不再与它冲突。具体 gate 见
`docs/plans/2026-07-21-product-form-and-roadmap.md` 第五节，确定性加固细节见
`docs/plans/2026-07-21-p0b-luau-hardening-ledger.md`。

### strict-background 的位置

产品的 `background_only` 是 fail-closed 契约：不兼容时必须失败，不能切换为
前台激活、全局注入或真实光标移动。该不变量目前由自动化/controller 路径承担，
不由 `script` 实现。`script` 没有 input API，也不依赖 Windows-only
`controller`，因此既不能违反也不能证明 strict-background。

未来 host binding 接入时，Luau 只能获得受限 capability，不得绕过 observation lease、target generation
或 controller 兼容性门；roadmap 还要求在 VM state 创建前验证 backend capability 与 target compatibility。

### 资源与取消不变量

以下机制现在都已在代码中存在（本节此前把它们列为「尚未成立」）：

- accounting allocator 与每任务内存硬配额（`ffi/allocator.{hpp,cpp}`）；
- instruction budget 与 `maxRuntime` 时间预算，均由 interrupt callback 读取；
- atomic cancel flag、interrupt callback、`lua_break()` 和 abandon protocol
  （`ffi/cancellation.{hpp,cpp}`）；
- `gc >= 0` 时禁止中断的 GC guard，作为 callback 的第一条语句；
- `luaL_sandbox()`，以及经 `deepFreeze` 实现的递归 readonly host tables；
- 对 `getfenv`、`setfenv`、`newproxy`、`gcinfo`、`coroutine`、`debug`、`_G` 的显式移除，
  外加残余时钟与 RNG 入口。

`luaL_sandboxthread()` **不在**这份清单上，而且永远不会在：见上文「两个环境」。

有一处代码已经编码、读者不该丢掉的细节：`lua_break()` 在 `nCcalls > baseCcalls` 时抛出的是
**普通可捕获错误**，不是 `LUA_BREAK`。所以「是否发生了硬取消」的真相是
`InterruptState::broken`，而不是 resume 状态码，`resumeChunkOnThread` 两者都查。把那种退化
的 break 归类成脚本错误，等于把宿主控制信号误报成可恢复失败。

hardening ledger 的硬红线要求取消使用 interrupt 中的 `lua_break()` 后 abandon coroutine，
绝不能使用可被 `pcall` 吞掉的 `luaL_error()`。它还要求长耗时 C++ binding 自己遵守
deadline/stop token；VM interrupt 无法抢占卡死的 C++ 调用。两者共同构成“500ms 总退出”而不是
单一 VM 技巧。其中「逐个 binding」那一半（一票否决第 6 条，人为阻塞每个原语）排在
`docs/plans/2026-07-29-three-layer-task-system.md` 的阶段 3，至今没跑过。

## 与产品运行时的关系

### 当前调用方

入站边是 `modules/task` 与测试。`modules/task` 是唯一的非测试消费方：它从
`capability-surface.hpp`、`framework-bundle.{hpp,cpp}` 和 `task-host.cpp` include
`<script/engine.hpp>`，经 `HostTableInstaller` 提供 `uf` 数据表，经
`PrivateCapabilityInstaller` 提供观察周期原语，经 `frameworkModules` 提供 `.luau` bundle。

没有 `entry/`、`engine`、`annotation` 或 `controller` 源文件直接 include
`script/engine.hpp`——`script` 是经由 `task` 传递地进入 CLI 的。这是一条可由仓库引用搜索
直接验证的边界。

### 当前依赖

`modules/script/manifest.txt` 声明：

- public `core`：公开函数返回 `Result<T>`；
- public `domain`：实现用 `AutomationErrorKind` 分类自动化失败，测试也消费该
  分类；
- private `Luau.VM Luau.Compiler`：Luau 类型不出现在 public header。

`cmake/build.cmake` 的 `cpp_define_module(...)` 在定义模块 target 前检查 `${MODULE_PATH}/external/CMakeLists.txt` 并
`add_subdirectory(... EXCLUDE_FROM_ALL)`；Pass 1 建立 `Luau.*` targets，Pass 2 才解析 private 依赖。

`modules/script/external/CMakeLists.txt` 强制关闭
`LUAU_BUILD_CLI`、`LUAU_BUILD_TESTS`、`LUAU_BUILD_WEB`，然后添加固定位置的
`modules/script/external/luau`。`.gitmodules` 把该路径登记为
`https://github.com/luau-lang/luau.git` submodule；集成计划记录的精确基线是
tag 0.730、commit `5bc7f4b23756f69f4669b419fa9034f117ccd6fe`。

### FFI 边界传递的内容

跨 Luau 边界的内容目前只有：

- 入站：源码 bytes、长度、compile options、chunk name；
- 中间：caller-owned bytecode buffer；
- VM 控制：`lua_State*`、load/resume status；
- 出站：一个 `double` 或仓库 `Error`。

没有 C++ domain object、raw screenshot、controller handle 或 callback 穿过
边界。这个极窄表面使当前 proof 容易审计，也为以后把宿主对象改成 opaque
handle 留出空间。

第三方头文件只在 `modules/script/source/script/ffi/engine.cpp` 中 include，
并用 MSVC/Clang/GCC 对应 pragma 局部压低第三方 warning。项目自己的 target
仍应用严格 safety profile；warning suppression 没有扩散到整个模块。

### 计划中的接入方式

产品计划要求 C++ 宿主持有截图、识别、输入、按键账本和 trace；Luau 只消费
最小、只读、可取消的 capability。这意味着未来跨边界的应是经过验证的句柄和
可序列化结果，而不是让脚本获得 controller、文件系统或 C++ raw pointer。

接线时还必须保持仓库模块图无环。当前 `script -> core, domain` 与
`engine -> core, domain, annotation` 彼此独立；任何新增依赖都必须由实际
composition 设计决定，不能仅为方便让 `script` 与 `engine` 相互依赖。

## 测试

宿主测试住在三个文件里：`tests/script/test-script.cpp`（底座）、
`tests/script/test-environments.cpp`（双环境拆分）、`tests/script/test-veto-suite.cpp`
（roadmap 一票否决）。`tests/CMakeLists.txt` 的 `test-script` target 链接
`${PROJECT_NAME}_script`，仅在模块存在时注册，并继承 60 秒 timeout 和 `CI` label。

`test-environments.cpp` 固定本文赖以成立的隔离论断：project 环境不持有否定名单上的任何
名字、它够不到 framework 环境、失败的 host-table installer 会带着自己的错误让 `create`
失败，以及——**从 framework 环境内部**断言——framework 模块无法在加载期捕获危险全局。最后
这条正是 `installSandbox` 把危险全局的剥除放在 bundle 加载**之前**而非之后的原因。

`test-script.cpp` 最初的六个 doctest case 固定以下行为：

- `Engine runs a Luau script and returns its numeric result`：验证
  `create()` 成功、compile/load/resume 正常路径和 `1 + 2 -> 3.0`。
- `Engine reports a compile/load error as a recoverable failure`：验证错误源码
  返回 failure，并分类为 `AutomationErrorKind::InvalidResource`。
- `Engine reports a runtime error as a recoverable failure`：验证脚本
  `error('boom')` 不穿出 C++ boundary，同样成为 `InvalidResource`。
- `Engine returns zero when there is no numeric result`：两个 subcase 分别固定
  无返回值和字符串返回值都得到 `0.0`。
- `Engine does not accumulate state across repeated runs`：同一 VM 执行 500 次仍稳定；只证明主栈不持续
  积累，不证明 globals 隔离。
- `Engine is move-only and usable after a move`：验证所有权移动后，目标
  `Engine` 仍能执行脚本。

sandbox、内存配额、取消、预算、globals 隔离和 `deepFreeze` 现在都已被固定——由
`test-script.cpp` 的后续 case 和覆盖一票否决第 1–5 条的 `test-veto-suite.cpp` 承担。这里仍
未固定的是一票否决第 6 条（人为阻塞每个长耗时 binding、总退出仍在预算内）、host binding
和 strict-background 行为；第一项排在阶段 3，后两项不由本模块证明。Luau upstream 测试也因
`LUAU_BUILD_TESTS=OFF` 不进入项目 CI。

roadmap 的六条 veto 必须补成 host regression suite：

1. 普通及嵌套 `pcall` 中的无限循环均在 500ms 总预算内不可恢复地停止；
2. 无限分配、深递归和重字符串操作只终止对应任务；
3. 文件、网络、进程、环境变量、动态加载与真实时钟不可访问；
4. 同一 observation trace 与 seed 重放 1000 次，action trace 和最终 state
   hash 完全一致；
5. 新 generation 编译/自检失败不影响旧 generation，成功切换不混用对象；
6. 每个长耗时 C++ binding 被人为阻塞时仍满足 cooperative cancel 总预算。

hardening ledger 进一步要求覆盖 nested host table freeze、sandbox 后仍存活的
五个 globals、GC interrupt guard、`table.sort` comparator/
`string.gsub` callback 等 non-yieldable context，以及“binding drain + 补 Up”
的端到端 500ms 测试。只有这些测试落地，0.730 的 spike 结论才从一次性证据
升级为仓库持续保证。

## 接入产品前的工作

扩展顺序由 `docs/plans/2026-07-21-p0b-luau-hardening-ledger.md` 管辖，而不是
由当前 `Engine` 的便利性决定。现有底座完成后，应按以下顺序继续：

1. ~~在 `modules/script/source/script/ffi/` 内补 sandbox setup~~——**已完成**，但形状与本条
   所述不同。host tables 已注册并递归 freeze，残余 globals 已移除，`luaL_sandbox()` 已执行；
   而每任务隔离用的是**两张显式环境表**，不是 `luaL_sandboxthread()`——后者已被否决，理由见
   「两个环境」。
2. 安装 accounting allocator，使内存配额成为 task-owned policy。OOM 本身是
   可被脚本捕获的普通错误；强制停机必须依赖 allocator hard quota 与宿主停止
   语义，不能把 `LUA_ERRMEM` 误当成不可吞取消。
3. 增加 cancellation/预算状态：其他线程只写 atomic 状态，VM owning thread
   的 interrupt callback 在非 GC 上下文调用 `lua_break()`，宿主收到
   `LUA_BREAK` 后销毁且永不 resume 该 coroutine。
4. 把 roadmap 六条 veto 和 ledger 的窄边界 case 先落为 `tests/script/` 回归；每次 Luau 升级重跑
   interrupt、sandbox 和 determinism suite，这也是精确 pin 0.730 的原因。
5. veto 通过后才加入 recognition/page 的只读 opaque handles，以及
   observe/act/wait host calls。其 schema 与动作证据应服从
   `docs/plans/2026-07-22-annotation-design.md` 和现有 engine 契约，而不是在
   Luau binding 中重新发明。
6. 最后再由组合入口把任务执行接到严格后台 controller 与 trace。
   VM 创建前完成 backend capability/target compatibility gate；每个长 C++
   binding 都必须有界且 cooperative-cancellable。

`docs/plans/2026-07-21-product-form-and-roadmap.md` 是产品级 authority：它定义
六条一票否决、P0 的 500ms Ctrl-C 目标、determinism/trace 和
`background_only`。`docs/plans/2026-07-21-lua-task-model-grill-decisions.md`
补充任务语义，尤其 D5 的 coroutine + interrupt 双预算、D9 的白名单沙箱，
以及“每任务全新 VM”的 P0 生命周期。

若 Luau 无法通过 sandbox、不可吞取消或 500ms 停止目标，roadmap 要求重开
独立 worker 选型；不能通过扩大“可信脚本”假设来绕过 veto。反过来，在这些
gate 通过前，现有 `Engine::runNumber()` 应继续保持最小验证接口，不应被包装成
看似完整的产品 runtime。
