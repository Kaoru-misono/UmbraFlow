# modules/core：平台无关能力内核

本文解释 `modules/core` 的职责、公开能力和扩展边界。它面向第一次进入仓库、
需要沿着真实接口定位实现并安全扩展子系统的开发者。架构总约束见
`docs/ARCHITECTURE.md` 的 “Core capability kernel”；下文以
`modules/core/source/core/` 的实际头文件和保留测试为准。

## 模块范围

`core` 是模块图最底部的平台无关叶节点。`modules/core/manifest.txt` 只有
`[module]` 与 `[build]`，没有 `[dependencies]`；因此其他模块可以依赖它，
它不能依赖任何项目模块。`scripts/check_modules.py` 会拒绝 `core` 声明 link
dependency，并检查整个模块图无环。这不是构建文件的偶然状态，而是内核能够被
`domain`、`vision`、`image`、`annotation`、`engine`、`script` 和 Windows-only
`controller` 共同复用的前提。

它拥有的是可移植、可审计、能消除一类无效状态的“小机制”：

- `error/`：可恢复失败、传播语法和 release 仍生效的契约终止。
- `control/`：遍历和 visitor 的显式继续/提前退出结果。
- `concurrency/`：把互斥锁与受保护值绑定的同步访问。
- `numeric/`：整数运算、整数转换和浮点到整数转换的显式失败。
- `safety/`：静态分析注解与连续存储的受检索引。
- `text/`：UTF-8 验证与 scalar 解码/编码，以及隔离的 code-unit 位转换。
- `types/`：固定宽度整数、强类型值、标识符、generation、非零值、flags 和
  显式枚举反射。
- `time/`：进程内单调时刻及其溢出安全运算。
- `utility/`：`std::variant` visitor 组合与确定性 scope cleanup。

不存在聚合 `core.hpp`。调用方必须 include 所需的精确 facility；这样依赖面、
编译成本和概念耦合都保持可见。应用 identity 留在 entry 层，不进入这个只包含
机制的叶节点。根据 2026-07-28 的开发者决定，仓库根 `manifest.txt` 向 CMake
提供应用名和版本，再生成仅供 entry 使用的 `application-info.hpp`。只有需要非模板
实现的 facility 才有匹配的 `.cpp`：`error/contracts.cpp`、`error/error.cpp`
和 `text/utf8.cpp`。

`core` 不包含产品语义。它不知道 frame、detection、annotation、engine
session、Windows handle、Luau、GUI 或具体游戏，也不决定失败是否应该 retry、
abort 或展示给用户。`AutomationErrorKind` 与 `FailureResponse` 位于
`modules/domain/source/domain/error.hpp`，严格后台捕获和输入位于
`modules/controller`；底层只提供稳定表示和检查，策略由业务 owner 制定。

它也不复制标准库。`Result<T>` 是 `std::expected<T, Error>` 的 alias，
`ControlFlow` 建在 `std::variant` 上，缺席仍用 `std::optional`，所有权仍用标准
智能指针和值类型。已归档的权威计划
`docs/archive/plans/2026-07-20-safe-cpp-core.md` 明确排除了 intrusive
references、自制容器、serialization、VFS、job system、task runtime、channel
和 profiling；这些不能因为“看起来通用”就进入 `core`。

## 基础能力

### error：失败的构造、分类和转发

`modules/core/source/core/error/error.hpp` 定义 move-only `Error`。对象本身只有
一个 `std::unique_ptr<Payload>` 数据成员；设计目标是让 `Error` 保持一个指针宽，
把诊断数据移到堆上。原因在成功路径：`Result<T>` 的 `std::expected` 必须为
`T` 与 `Error` 的二选一存储付空间成本。小型 `Error` 避免每个成功返回值都内联
携带多个 `std::string`、`std::vector` 和 `std::source_location`；分配只发生在
需要构造错误时。源码没有用 `static_assert(sizeof(Error) == sizeof(void*))`
声明 ABI 保证，所以应把“一指针”理解为当前布局意图，不是跨实现序列化契约。

`Error::Payload` 保存五类信息：`m_detailCode` 是机器可分支的
`std::error_code`；`m_nativeCode` 是保留自身 category 的可选 OS/库错误；
`m_message` 是人类可读说明；`m_location` 默认由
`std::source_location::current()` 捕获构造点；`m_context` 是错误上穿语义层时
按顺序追加的上下文字符串。

分类不靠解析 `message`。`detailCode()` 返回的 `std::error_code` 同时携带整数值
与 category；category 就是错误词汇表的身份。`core` 的
`fail(std::error_code, ...)` 接受任何词汇表，而不会再增加一个较粗的通用 code。
例如 `modules/domain/source/domain/error.cpp` 定义私有
`AutomationErrorCategory`，把 `AutomationErrorKind` 编进
`uf.automation` category，再由 domain 的 `fail(AutomationErrorKind, ...)`
转调 `uf::fail`。调用方可以精确识别 domain 错误，同时 `nativeCode()` 仍能独立
保留 `std::system_category()` 的平台原因。分类、原生原因和说明各司其职，避免
把 “what failed” 与 “which API returned what” 压成脆弱字符串。

`Error` 禁止复制并允许 noexcept move。传播时只有一个 owner，不会出现下层
frame 与上层 frame 共享同一份可变 context 的隐式别名；确需副本时必须显式调用
`clone()`，它深复制整个 `Payload`。move 后的对象只允许析构或重新赋值：
`payload()` 与 `addContext()` 都以 release-active `UF_CHECK` 检查
`m_payload != nullptr`，错误使用会终止而不是读取空指针。

堆载荷还带来稳定地址：移动 owning `Error` 不会移动 `Payload`，所以
`message()` 的 `std::string_view` 和 `context()` 的 `std::span` 可跨
`Error` move 保持有效。它们仍是 borrow：payload 析构后全部失效，
`context()` 还会被下一次 `addContext()` 可能触发的 vector reallocation
失效。声明上的 `UF_LIFETIME_BOUND` 和相邻 `SAFETY` 注释把这份 lifetime
contract 暴露给分析器与读者。

`toString(Error const&)` 是最终诊断渲染器：输出 detail category 名和
category message、自由文本、构造位置，可选 native category/value/message，
再按插入顺序输出每一层 context。它不记录日志；传播与可观测性边界由上层决定，
因而同一个失败不会在每一层被重复打印。

`modules/core/source/core/error/result.hpp` 提供：

- `Result<Value>`：`std::expected<Value, Error>`。
- `Status`：`Result<void>`。
- `fail(...)`：构造 `std::unexpected<Error>`。
- `ok()`：构造成功的 `Status`。
- `withContext(result, text)`：仅在失败分支原地追加 context，再按值返回结果。

`UF_TRY(expression)` 先以 `auto ufResult = (expression)` 按值持有结果。它不绑定
`auto&&`，因此不会让宏继续读取一个临时 `Result` 内部的悬空引用。失败时表达式
`std::move(ufResult).error()` 保持 xvalue，`std::unexpected` 取得 move-only
`Error` 的所有权并立即从当前函数返回；成功时不做额外工作。若传入 lvalue
`Result`，调用方必须显式 `std::move`，这让消费所有权可见。

`UF_TRY_CONTEXT` 的差别仅是先把 error move 到局部 `ufError`，追加本层 context，
再 move-return。`UF_TRY_VALUE(name, expression)` 在成功时执行
`auto name = *std::move(result)`，把 value 移入调用方当前 braced block；
`UF_TRY_VALUE_CONTEXT` 同时提供失败上下文。两个 value 宏是 declaration-style
宏，必须作为带花括号作用域中的独立 statement 使用；内部结果名由
`__LINE__` 拼接，避免常见局部名冲突。四个宏都只转发原 `Error`，不会重分类、
clone、log 或捕获异常。

不可恢复的 programmer defect 走另一条通道。
`modules/core/source/core/error/contracts.hpp` 定义 `UF_ASSERT`/
`UF_ASSERT_MSG`、`UF_CHECK`/`UF_CHECK_MSG` 与 `UF_UNREACHABLE`/
`UF_UNREACHABLE_MSG`。`UF_ASSERT` 在 `NDEBUG` 下不求值，只保留
`sizeof` 级语法检查；`UF_CHECK` 和 `UF_UNREACHABLE` 在 release 仍调用
`detail::contractViolation`。实现尝试把 kind、expression、message 和
`source_location` 写到 `std::cerr`，即使诊断自身抛异常也最终 `std::abort()`。
因此 contract failure 不是可恢复 `Error`，调用方不能继续执行已破坏不变量的状态。

### control 与 utility：显式控制流和作用域行为

`modules/core/source/core/control/control-flow.hpp` 的 `Continue<Value>` 与
`Break<Value>` 拥有自己的 `value`，并以 `static_assert` 禁止 reference
payload；默认 payload 是 `std::monostate`。`ControlFlow<BreakValue,
ContinueValue>` 是二者的 `std::variant`，`isBreak()` 和 `isContinue()` 用
alternative 类型判断状态。它表达 visitor/traversal 的局部提前退出，不是
coroutine、任务取消或异常替代品。

`modules/core/source/core/utility/variant-match.hpp` 的 `Overload` 组合多个
callable 的 `operator()`，`matchVariant()` 再交给 `std::visit`。所有 variant
alternative 必须在编译期可调用，facility 只消除 visitor 样板，不引入另一套
sum type。

`modules/core/source/core/utility/scope-exit.hpp` 的 `ScopeExitFunction` 要求
callable 可 noexcept move 且可 noexcept invoke。`ScopeExit` move-only，
move construction 用 `std::exchange` 解除源对象，确保 cleanup 恰好由一个对象
负责；析构时若仍 armed 就调用，`release()` 可显式取消。`scopeExit()` 只接受
rvalue callable，避免把外部 callable 本身当作隐式存储 borrow。

### concurrency：锁与状态不可分离

`modules/core/source/core/concurrency/synchronized.hpp` 的
`Synchronized<Value, Mutex = std::mutex>` 同时拥有 `m_value` 和 mutable
`m_mutex`，自身既不可复制也不可移动。默认构造会 value-initialize `Value`；
还支持按值构造和 `std::in_place` parenthesized construction。

读写只能经过 `withLock(function)`。non-const overload 把 `Value&` 交给 callback，
const overload 交 `Value const&`，两者都在 `std::lock_guard` 生命周期内调用。
返回类型可为值或 `void`，但 `k_lockResultDoesNotExposeStorage` 在编译期拒绝直接
返回 pointer 或 reference，防止最明显的锁外 alias。它不能证明 callback 没把
地址藏进另一个对象，因此它缩小而没有夸大 C++ 能强制的安全范围；跨线程存储的
callback lifetime、取消和 join 仍由拥有并发工作的上层负责。

### numeric：先证明运算有效，再执行

`modules/core/source/core/numeric/checked-arithmetic.hpp` 的 `CheckedInteger`
接受 integral，但排除 `bool`、普通 `char`、`wchar_t` 与各 Unicode character
type，避免把逻辑值或 code unit 当作数量运算。

`checkedAdd`、`checkedSubtract`、`checkedMultiply`、`checkedDivide` 和
`checkedRemainder` 返回 `std::optional<Value>`。每个函数在执行可能产生
undefined behavior 或无符号 wrap 的表达式前先检查边界。signed multiply 先由
`detail::unsignedMagnitude` 在无符号域求绝对量，分别使用正上限或负下限的
magnitude，因此能接受 `min * 1`，同时拒绝 `min * -1`。divide 与 remainder
都拒绝零除和 signed `min / -1` 特例。

`modules/core/source/core/numeric/checked-cast.hpp` 分开两种语义。
`checkedCast<To>(integer)` 用 `std::in_range` 拒绝符号变化或 narrowing；
`checkedIntegralCast<To>(float)` 先拒绝 NaN/Infinity，再用 `long double`、
`numeric_limits<To>::digits` 与 `std::ldexp` 构造精确的半开范围，最后拒绝任何
fractional value。所有失败都是 `std::nullopt`，由拥有业务语义的调用方决定它是
输入错误、资源错误还是 contract defect。

### safety 与 text：borrow 可见，字节边界隔离

`modules/core/source/core/safety/annotations.hpp` 把 Clang 的 lifetime、
no-escape、unsafe-buffer 与 thread-safety attributes 包装成 `UF_*` 宏；非 Clang
编译器上宏为空。这些是分析增强，不改变运行时语义，也不能单独证明 lifetime。

`modules/core/source/core/safety/checked-access.hpp` 的 `tryAt(span, index)`
越界返回 `nullptr`，`checkedAt(span, index)` 越界触发 release-active
`UF_CHECK`。range overload 只接受 contiguous、sized、可构造 `std::span` 的
lvalue range；`CheckedAccessRange` 明确拒绝 temporary owner，避免返回指针或
引用后 owner 已析构。返回值仍是只在本次调用上下文中有效的借用，调用方不能在
容器失效后保留。

`modules/core/source/core/text/utf8.hpp` 的 `isValidUtf8()` 接受空串和合法的
1–4 byte scalar encoding，拒绝孤立 continuation、overlong sequence、截断序列、
surrogate 与大于 `0x10FFFF` 的值。`decodeUtf8Scalars()` 复用同一状态机，返回
解码后的 `uint32` scalars，输入畸形时返回 `std::nullopt`。`appendUtf8Scalar()`
覆盖四种编码宽度；调用者若传入 surrogate 或越界 code point 会触发 `UF_CHECK`，
因为函数前置条件已经声明参数必须是 Unicode scalar。

原始 code-unit 表示转换集中在
`modules/core/source/core/text/unsafe/unicode-code-unit.hpp`：
`utf8CodeUnitValue`、`utf8CodeUnit` 和 `utf16CodeUnitValue` 使用
`std::bit_cast` 保留对象表示，避免 `char` signedness、narrowing 和 aliasing
含混。每个操作都有局部 `SAFETY` 论证，普通 UTF-8 算法只消费安全的整数值。

### types 与 time：建立不会混淆的词汇表

`modules/core/source/core/types/integer.hpp` 给出 `int8` 至 `uint64`、
`intptr`/`uintptr` 和 `intmax`/`uintmax`，让跨模块宽度意图直接出现在接口中。

`modules/core/source/core/types/strong-value.hpp` 的
`StrongValue<Tag, Representation>` 通过不同 `Tag` 生成互不兼容的类型。构造是
explicit 且没有默认构造；相同 representation 的两个 domain value 不能隐式互转。
scalar `value() const&` 按值返回，非 scalar 按 `const&` 返回，rvalue `value()`
move 出 representation。`StrongValueHash` 提供显式 opt-in hashing。

`modules/core/source/core/types/strong-id.hpp` 的 `StrongId<Tag,
Representation = uint64>` 只是约束 representation 为 unsigned 的
`StrongValue` alias，不增加虚构的 ID 生命周期策略。`Generation<Tag,
Representation>` 则提供 `initial()`、`fromValue()`、`value()` 和 `next()`；
到最大值时 `next()` 返回 `std::nullopt`，绝不 wrap 后复用旧 generation。

`modules/core/source/core/types/non-zero.hpp` 的 `NonZero<Value>::create()`
只在非零时返回对象，私有构造器保证成功构造后不变量持续成立。
`modules/core/source/core/types/flags.hpp` 的 `Flags<Enum>` 只接受 unsigned
underlying enum，提供 `bits`、`empty`、`containsAll`、`containsAny`、
`insert`、`remove`、`with`、`without` 以及集合间 `|`/`&`；没有 raw-bit
constructor 或无界 complement，避免悄悄引入枚举未声明位。

`modules/core/source/core/types/enum-reflection.hpp` 通过
`UF_REFLECT_ENUM` 显式注册 enumerator。`EnumTraits` 保存静态
`EnumEntry` array，consteval validation 拒绝空集合、空名称、重复 value 和重复
name；`enumEntries()`、`enumName()`、`enumFromName()` 支持 sparse enum，
未知映射返回 `std::nullopt`。宏生成的 label 来自 C++ token，重命名 enumerator
也会改变 label，所以它不是无需版本化的持久化 wire contract。

`modules/core/source/core/time/monotonic-time.hpp` 的 `MonotonicInstant` 封装
`std::chrono::steady_clock::time_point`。`now()` 获取进程内单调时刻，
`fromTimePoint()` 为确定性测试提供注入点，`checkedAdd()` 用 checked integer
addition 报告溢出，`saturatingDurationSince()` 对倒序时刻返回 zero，对无法表示
的正差返回 `Duration::max()`。它用于 timeout、age 和 interval，不是墙钟，也不是
可跨进程、跨机器或落盘的 serialization type。

## 必须保持的约束

**Fail-closed。** 所有危险边界都先拒绝再继续：受检运算返回空值而不 wrap，
`tryAt` 越界返回空指针，`checkedAt` 与失效 contract 直接终止，UTF-8 validator
拒绝非 canonical scalar encoding，generation exhaustion 不回绕。`Error` 若不
属于上层已知 category，上层可以保守分类；具体例子是
`modules/domain/source/domain/error.cpp` 的 `failureResponse(Error const&)`
对无法识别的 detail category 返回 `Abort`。`core` 提供做出 fail-closed
决策所需的无歧义表示，但产品策略仍留在 owner 模块。

**Determinism。** checked numeric 不依赖 UB 或实现定义 overflow；
enum mapping 是显式、编译期验证且线性按注册顺序查找；error context 与
`toString()` 按追加顺序渲染；`ScopeExit` cleanup 恰好由当前 armed owner 执行。
`MonotonicInstant` 避免墙钟跳变，并提供构造固定 `TimePoint` 的测试入口。
这不意味着 `now()` 或线程调度可重放，而是把可控机制中的非确定性源隔离出去。

**Ownership 与 lifetime。** `Error`、`ScopeExit` 与 `Synchronized` 都禁止隐式
复制；前两者通过 move 转移唯一责任。`Continue`/`Break` 不允许 reference
payload，checked range access 不接受 temporary owner，`Synchronized::withLock`
不允许直接返回 pointer/reference。view 接口使用 `UF_LIFETIME_BOUND` 并注明
失效条件。机制优先让 owner 出现在类型和作用域里，而不是依靠命名暗示安全。

**Strict-background。** 这不是 `core` 的运行时不变量。`core` 不接触窗口、
capture API 或 input delivery，无法判断动作是否落到背景目标。它的贡献是让
controller 能以 `Result` 返回类型化失败、用 `MonotonicInstant` 检查时效、用
checked numeric 避免坐标溢出，并在内部不变量破坏时终止；“绝不回退到前台输入”
由 `modules/controller` 和组合根执行。把策略留在平台 owner 才能让 `core`
继续保持可移植。

## 被哪些模块使用

入边从所有消费者指向 `core`，出边只有 C++23 标准库；没有项目模块类型跨入
`core` API。跨边界传递的是值、标准 view、`Result<T>`/`Error`、
`MonotonicInstant` 和小型模板词汇，而不是 platform handle 或业务对象。

典型协作链如下：

1. `domain` 用 `StrongId`/`Generation` 建立 `FrameId`、`TaskRunId`、
   `TargetGeneration` 等互不混淆的词汇，用自有 error category 把
   `AutomationErrorKind` 装入 core `Error`。
2. `vision`、`image` 和 `annotation` 用 checked arithmetic/cast/access
   处理尺寸、stride、offset 和索引，用 UTF-8 与 enum helper 做确定性验证，
   失败通过 `Result` 上送。
3. `engine` 的 ports 和 session 只交换平台无关对象与 `Result`，用单调时间处理
   observation age；它不要求 `core` 认识 capture 或 action policy。
4. Windows `controller` 在平台边界产生 native `std::error_code`，同时保留
   domain detail classification；strict-background 判断属于 controller。
5. `entry/` 组合各模块，在用户/日志边界调用 `toString()` 或把结构化字段写入
   trace，而不是让底层传播宏自行记录。

这种单向关系解释了为什么 `core` 不能依赖 `domain`：若通用 `Error` 直接包含
`AutomationErrorKind`，每个非自动化用途都会被产品词汇污染，且
`domain -> core` 会形成反向边。正确扩展方式是由 owner module 建 category、
classifier 和更便利的 `fail` overload。

## 测试

`tests/CMakeLists.txt` 把以下七个文件组成 `test-core`，链接
`${PROJECT_NAME}_core`，以 C++23 和仓库 safety profile 编译，并带 `CI` label：

- `tests/core/test-error.cpp` 固定 `Error` 不可复制、可 noexcept move、clone
  深复制、detail/context/native rendering，以及四类 `UF_TRY*` 的成功提值、
  失败原样 move 和 context 追加行为。
- `tests/core/test-checked-arithmetic.cpp` 固定 unsigned underflow/overflow、
  signed min/max、multiply 极值、零除、`min / -1`、narrowing、fractional、
  NaN 与 Infinity 的拒绝边界。
- `tests/core/test-strong-types.cpp` 用 compile-time assertions 固定 ID 无默认/
  隐式转换、不同 tag 不同类型、scalar/non-scalar `value()` 返回形态，并测试
  hashing、ordering 和 generation exhaustion。
- `tests/core/test-capabilities.cpp` 固定 variant exhaustive visitor、sparse enum
  round-trip、`ControlFlow` payload、`NonZero`、`Flags`、`ScopeExit` exactly-once
  语义，以及 `Synchronized` 的 4,000 次并发更新、value initialization 和
  in-place overload resolution。
- `tests/core/test-checked-access.cpp` 在编译期拒绝 temporary vector，并在运行时
  固定 mutable/const access 与越界 `nullptr`。
- `tests/core/test-utf8.cpp` 覆盖四种 UTF-8 宽度以及孤立 continuation、
  overlong、truncated、surrogate 和超最大 scalar。
- `tests/core/test-monotonic-time.cpp` 固定 checked addition overflow、倒序归零
  与不可表示 duration 差值饱和。

契约终止和 annotation 宏没有在 `test-core` 中伪装成普通可恢复路径；它们还受到
编译、下游使用和仓库 gate 约束。`scripts/check_safety.py` 检查危险操作只能位于
`unsafe`/`platform`/`ffi`/`external` 边界且附近有 `// SAFETY:`，
`tests/test-check-safety.py` 固定该 gate 的规则；`scripts/check_modules.py`
固定 `core` 无依赖与模块图无环。扩展某个 facility 时，应把最小边界行为补进
对应测试文件，而不是只依赖集成测试间接覆盖。

## 扩展规则

新增能力时，优先增加职责单一的 header，让调用方只 include 所需概念，不要扩大
现有类型。若需要非模板实现，遵循“一个实现文件对应一个 header”；不要添加
聚合 `core.hpp`。错误词汇扩展应发生在 owner module 的 `std::error_category`
和 classifier；`Error` 已经提供 detail/native/context 扩展点，无需增加产品 enum。

已归档计划 `docs/archive/plans/2026-07-20-safe-cpp-core.md` 是当前 kernel 范围的
历史权威：generational `SlotMap`、`Signal` 是 product-level candidates，
structured `TaskGroup`/bounded `Channel` 只有在真实异步编排需求出现后才考虑，
serialization、VFS、job system 等仍明确留在共享 core 外。当前
`docs/plans/2026-07-21-luau-integration-plan.md` 把 script 模块声明为
`core domain` 的消费者，并复用 `Result`/`Status`/`fail`；它没有授权把 Luau
runtime、取消策略或脚本错误表推进 `core`。

评估新能力时遵循仓库的 `evaluate-core-capability` 流程：先找至少两个真实
call site 或可测量需求，确认 C++23 标准库不能同样清晰地解决，再证明新类型确实
删除 invalid state、lifetime hazard 或重复控制流；最后只提升可移植、可审计、
有简短 retained test 的最小 contract。标准反射成熟后，
`EnumTraits` 的 backend 可以替换而保持 `enumName`/`enumFromName` 调用面；
除此之外，计划中的 product policy 应优先进入拥有它的模块，而不是借
“通用能力”之名穿透 `core` 边界。
