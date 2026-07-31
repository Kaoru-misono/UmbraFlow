# Annotation 后端分支评审报告

> **词汇重定向(2026-07-31)。** 本文是有日期的记录,不改写。下文的
> `recognizer` / `RecognizerId` / `uf.recognizers` / `recognizerId` 一律读作
> **element** / `ElementId` / `uf.elements` / `elementId`;`RecognizerDefinition`
> 与 `RecognizerVariant` 读作 `CompiledElement` 与 `CompiledAppearance`;
> `Variant` / `variant` 读作 `Appearance` / `appearance`。`RecognitionCatalog` 与
> `RecognitionRuntime` 名字不变——它们指的是「识别」这个动作。schema id 随改名一起动了:
> `umbraflow-authoring/v4`、`umbraflow-annotations/v3`、`umbraflow-trace/v2`。
> 权威词汇见 `CONTEXT.md` 的「Annotation model」一节。授权文档里的 `[[annotation]]`
> 表在同一次 v4 升版里改名 `[[element]]`。

> 状态:已完成并归档(2026-07-24)——§4 决策项已于 2026-07-23 决定并落地，低优先记录已明确留档而不纳入修复。

- 评审范围：`origin/master..HEAD`，即 `08ca93d` 之后的 14 个提交
- 评审日期：2026-07-22
- 评审方式：多智能体分维度评审，每条候选发现由 3 个独立视角对抗性反驳后存活才保留
- 代码基线：`6e26ec8`（评审时的 HEAD）

## 1. 总览

这批提交在 `modules/annotation` 下建立了完整的标注识别栈——识别契约与目录校验、运行时清单与模板资产的生成与解析、有界页面识别运行时、静态回归执行器、坐标动作授权门——并新增 `entry/workbench` 的原子文件发布与项目持久化、`entry/workbench` 的编辑历史，同时把 PNG 编解码从 `entry/m0-demo` 抽取为独立的 `modules/image`。

**整体结论：架构分层清晰，所有权表达干净，错误处理基本统一走 `Result<T>`，未发现内存安全缺陷、未定义行为，也未发现会产生错误输出的逻辑错误。**

健康问题集中在两处：

1. **外部输入的错误分类泄漏了内部不变量错误码**（唯一 high）。
2. **信任边界的拒绝分支缺乏负向测试**——`catalog.cpp` 与 `authorization.cpp` 合计约 25 个 `return fail(...)` 可以被整段删除而全套 `ctest -L CI` 依旧通过。这是本次提交序列最实质的回归风险。

两者均已修复，详见 §3。

### 统计

| 项目 | 数量 |
| --- | --- |
| 原始候选发现 | 91 |
| 通过三轮对抗验证 | 43 |
| 验证阶段被反驳剔除 | 48 |
| 合并重复后的独立问题 | 29 |

43 条存活发现中有大量跨维度重复（`parsePixelRectField` 一处缺陷被 6 个维度各自报出，另有 5 处各被报 2–4 次），合并后为 29 个独立问题：

| 严重度 | 独立问题 | 已修复 | 留待决策 |
| --- | --- | --- | --- |
| critical | 0 | — | — |
| high | 1 | 1 | 0 |
| medium | 5 | 4 | 1 |
| low | 21 | 12 | 9 |
| 提交历史 / 文档 | 2 | 1 | 1 |
| **合计** | **29** | **18** | **11** |

以上统计冻结于 2026-07-22 的评审结论；后续采纳情况记录在 §4，
不回写评审当时的计数。

评审覆盖 14 个提交所触及的全部已提交文件，围绕正确性、项目规则（CLAUDE.md 常开安全红线、`cpp-coding` 全套参考、`docs/ARCHITECTURE.md`）、所有权与生命周期、外部输入健壮性、测试有效性、过度设计与重复、提交历史卫生展开。

## 2. 评审方法与可信度说明

本次评审的每条发现都经过三个独立视角的对抗性验证，验证者被要求**默认反驳**，只有无法反驳时才确认：

1. **代码是否真的这么写**——重新打开文件核对引文，检查上下文是否已有守卫。
2. **引用的标准是否真实且适用**——核对项目规则原文、C++23 语义、TOML/PNG/SHA-256/Win32 契约。
3. **是否真的会发生、是否重要**——从真实入口追可达性，剔除纯风格问题。

两票反驳即淘汰。91 条候选中有 48 条被剔除（53%），被剔除的典型原因包括：引用了并不存在的项目规则、误述 C++ 语义、上游校验已使该状态不可达、以及把纵深防御误判为缺陷。

**修复的验证方式**：每一处修复都做了变异测试——先确认新测试在缺陷存在时**失败**，再确认修复后通过。未通过这一验证的测试不算数（一个在缺陷存在时也能通过的测试没有任何价值，这恰恰是本次评审发现的主要问题类型）。

## 3. 已修复

### 3.1 high：`parsePixelRectField` 对畸形外部输入泄漏 `InternalInvariant`

`modules/annotation/source/annotation/authoring-document.cpp:260`

**现象**：该函数直接返回 `PixelRect::create(...)` 的结果。`PixelRect::create` 是 domain 内部不变量工厂，对退化矩形（宽或高为 0）和坐标溢出一律报 `AutomationErrorKind::InternalInvariant`，经 `genericErrorCode` 映射为 `ErrorCode::Internal`。而 `parseAuthoringDocument` 这条解析链上其他**所有**失败都产出 `InvalidResource` / `ErrorCode::InvalidArgument`。

触发方式：把规范文档中任一 `[[annotation]]` 的 `template_rect = [1, 1, 1, 1]` 改成 `template_rect = [1, 1, 0, 1]`。文档仍被正确拒绝，但错误种类是「宿主内部 bug」而非「用户文件非法」。`template_rect` 与 `search_roi` 两个字段都走这条路径。

同模块的兄弟解析器 `runtime-manifest.cpp:61-73` 对**同一字段**做了正确转换——这是复制分叉，不是刻意设计。

**范围限定**：`parseAuthoringDocument` 目前没有生产调用方，调用方全是测试。文档仍是 fail-closed 被拒绝的，不存在崩溃、UB 或错误解析结果。危害限于公共 API 的错误分类契约，以及一旦接上「打开项目」流程后把用户可修复的文件损坏报成宿主 bug。

**修复**：照搬 `runtime-manifest.cpp` 的形状，显式转换为 `invalidAuthoring`。并在 `tests/annotation/test-authoring-document.cpp` 的拒绝表补入零宽矩形变异用例。

**验证**：先回退修复、保留新用例，`test-annotation` 失败并报 `CHECK( 12 == 2 )`（12 = `InternalInvariant`，2 = `InvalidResource`）；恢复修复后通过。

同时核查了同类模式的其他站点，确认**没有**第二处：`ProjectFingerprint::create` 与 `TemplateOffset::create` 本身即返回 `InvalidResource`，直接透传是正确的。

### 3.2 medium：`catalog.cpp` 的 17 个拒绝分支零测试

`modules/annotation/source/annotation/catalog.cpp`

**现象**：`RecognizerDefinition::create`、`PageSignature::create`、`RecognitionCatalog::create` 三个校验工厂合计约 22 个拒绝分支，而 `tests/annotation/test-catalog.cpp` 全文只有一个目录构造失败断言。

原因是结构性的：测试套件通往这些工厂的唯一入口 `tests/annotation/test-helpers.hpp` 的 `test::recognizer` / `test::page` 都以 `REQUIRE(result.has_value())` 结尾，**从构造上就无法观察拒绝**。

**可证伪的回归示例（评审期实测）**：注释掉 recognizer 名称唯一性循环后，`test-annotation` 全部 830 条断言依旧通过。此时两个同名 recognizer 可以进入目录，而 `authorization.cpp:52` 依赖名称唯一来绑定动作目标。

**修复**：在 `tests/annotation/test-catalog.cpp` 新增三个测试用例、共 17 个负向场景，**绕过** `test-helpers.hpp` 的 REQUIRE-成功包装，直接调用工厂：

- `RecognizerDefinition::create` 7 例：几何越出工程分辨率、模板大于 search_roi、非 action_target 声明默认点击、默认点击越出模板、page_anchor 携带 allowed pages、action_target 未授权任何页、重复 allowed page。
- `PageSignature::create` 2 例：重复 recognizer ID、required 与 forbidden 重叠。
- `RecognitionCatalog::create` 8 例：recognizer ID 重复、recognizer 名称重复、页 ID 重复、页名称重复、跨类全局 ID/名称冲突、页签名引用未知 recognizer、页签名引用非 page_anchor、allowed_page_ids 指向未知页。

**每个用例都断言具体的错误消息而非仅断言错误种类**。这一点是刻意的：这些分支全部返回 `InvalidResource`，只断言种类的话，某个用例在更早的守卫处被拒也会「通过」，测试就会为错误的理由变绿。断言消息把每个用例钉死在它要覆盖的那一个分支上。

**验证**：中和名称唯一性守卫后，新测试失败并报 `logged: recognizer names must be unique`；恢复后通过。

### 3.3 medium：`authorization.cpp` 授权门的 4 个拒绝分支零覆盖

`modules/annotation/source/annotation/authorization.cpp`

**现象**：`tests/annotation/test-authorization.cpp` 是全仓库唯一使用这些 API 的地方，原本只有两个负向断言。未覆盖的包括 **allowed-page 授权门**——决定动作 recognizer 是否可在已解析页上触发，是本模块的核心安全判定。

**可证伪的回归示例**：整段删除 allowed-page 检查后全套测试仍通过，即「限定只能在页 B 触发的 action recognizer，于页 A 在屏时也被授权点击」不会被任何测试发现。

**严重度定位**：目前尚无生产代码 include `annotation/authorization.hpp`，所以这是**尚未接线的安全契约上的回归暴露**，不是活的安全漏洞。

**修复**：新增两个测试用例覆盖 4 个分支：

- allowed-page 授权门：构造双页 fixture（home/away 两个 page_anchor、两个页签名），action recognizer 只授权 away 页，而帧证据解析为 home 页，断言 `ActionRejected` 且消息为 `action recognizer does not authorize the resolved page`。
- 目录缺失 action recognizer；
- 项目身份不匹配（结构相同但 `ProjectId` 不同的目录）；
- `ActionDetection::create` 绑定到 page_anchor 而非 action_target。

同样全部按消息钉死分支——这四个守卫都返回 `ActionRejected`，仅断言种类无法区分。

**验证**：中和 allowed-page 守卫后测试失败；恢复后通过。

### 3.4 medium：`tests/workbench` 中的 `reinterpret_cast` 位于安全边界之外

`tests/workbench/test-project-persistence.cpp`

CLAUDE.md 要求转换必须位于 `unsafe/` / `platform/` / `ffi/` 边界内且有临近 `// SAFETY:` 注释；`safety-profile.md` 直接把 `reinterpret_cast` 列为普通源目录禁止操作。原代码满足了注释那一半，边界那一半没有。

这不是 UB——`std::byte*` ↔ `char*` 是 C++23 明确允许的别名——但它新立了一条先例，且 `scripts/check_safety.py` 的 `SOURCE_ROOTS` 只含 `("modules", "entry")`，**门禁从不扫描 `tests/`**，所以规则被静默违反。

**修复**：调换两个 helper 的依赖方向，让 `readText` 成为原语（直接读入 `std::string`，无需转换），`readBytes` 经 `std::as_bytes` 派生。两处 `reinterpret_cast` 与两条 `NOLINTNEXTLINE` 全部消除。

### 3.5 low：可能打断 clang-analysis CI 车道的裸指针算术

`modules/annotation/source/annotation/detail/canonical-toml.cpp:209`

`auto const* const last = first + encoded.size();` 是 `safety-profile.md` 明令禁止的指针算术，且是全仓库唯一一处——其他所有 `from_chars` 站点都用 `std::to_address`。表达式本身对 `string_view` 良定义、无 UB。

但 `.clang-tidy` 启用了 `cppcoreguidelines-pro-bounds-pointer-arithmetic` 且 `WarningsAsErrors: '*'`，`cmake/compiler-safety-analysis.cmake` 又配了 `-Wunsafe-buffer-usage` 加 `-Werror`——**必需的 clang-analysis CI 车道很可能因此失败**。

**修复**：改用 `std::to_address(encoded.begin())` / `std::to_address(encoded.end())`，与仓库既有写法一致。

> **未在本机验证**：Windows 主机上没有 clang-analysis preset 的工具链，此项修复消除了违规写法，但 `linux-analysis` 车道是否还有其他失败点未经实跑确认。

### 3.6 其余已修复项

| 项 | 位置 | 处理 |
| --- | --- | --- |
| `UF_CHECK_MSG(false, ...)` 收尾穷尽 switch | `runtime-manifest.cpp:301` | 改为 `UF_UNREACHABLE_MSG`，与字节相同的 `authoring-document.cpp` 一致（`coding-standard.md:367`） |
| 临时文件永久携带 `FILE_ATTRIBUTE_TEMPORARY` | `windows-file-publication.cpp` | 改为 `FILE_ATTRIBUTE_NORMAL`，与同文件另一站点及 `windows-file-writer.cpp` 一致 |
| `CREATE_NEW` 缺 `FILE_FLAG_OPEN_REPARSE_POINT` | `windows-file-publication.cpp` | 补上；是四个一方 Win32 create/open 站点中唯一无理由缺失的，预置 reparse point 会被既有的 64 次重试循环自然吸收 |
| 手写可解除作用域守卫重复 core 能力 | `windows-file-publication.cpp` | `TemporaryFileCleanup` 类替换为 `core/utility/scope-exit.hpp` 的 `scopeExit`，与 `windows-capture.cpp` 既有模式一致（`core-reuse.md:28`） |
| 9 个文件使用整数别名但未直接 include | 见下 | 补 `<core/types/integer.hpp>`（`coding-standard.md:238-239`） |
| `ARCHITECTURE.md` 模块图缺边 | `docs/ARCHITECTURE.md:13` | 补 `annotation -> image` 私有边与整个遗漏的 `script` 模块，并注明图中省略 vendored 第三方目标 |

补 include 的 9 个文件：`catalog.cpp`、`content-hash.cpp`、`recognition.cpp`、`recognition-runtime.cpp`、`regression-runner.cpp`、`template-asset.cpp`、`image/pixels.cpp`、`entry/workbench/project-persistence.cpp`、`tests/annotation/test-template-asset.cpp`。均为本分支新增文件。

### 3.7 `6e26ec8` 编辑历史提交的修复

该提交（`entry/workbench/authoring-edit.*`）单独做了一轮评审。**撤销/重做状态机本身是正确的**：它用两个快照栈、不带游标索引，从结构上消除了大部分经典缺陷点；逐项核对了「新编辑后清空重做栈」「空栈撤销/重做」「容量裁剪丢弃最旧项」「撤销保真度」「异常安全」「自赋值与移动语义」，均未构造出可破坏其不变量的调用序列。

问题全在测试侧，已全部修复：

| 问题 | 修复 |
| --- | --- |
| 「拒绝非法草稿不改变历史」用例跑在空历史上，`CHECK_FALSE(canRedo())` 不可能失败 | 改为先建立一个待重做条目，再施加非法草稿，断言两个栈都完好且重做仍可执行 |
| 无用例覆盖「有待重做条目时施加空操作编辑」 | 在既有用例中补齐 |
| `redo()` 全套只被调用一次，空栈守卫与 LIFO 顺序均未验证 | 新增用例：空历史 redo、两级撤销后按逆序重做、重做耗尽后再 redo |
| `document()` 访问器全仓库零调用 | 在撤销后断言 `serializeAuthoringDocument(history.document())` 与原始文档一致，既覆盖访问器也钉住撤销保真度 |
| fixture 只用三个字段的零值分支，`makeAuthoringDraft` 丢字段也不会被发现 | 新增 `variantDocument()`：WGC provenance、`Negative` 分类、`UnknownRegression` 期望、非空 forbidden 集，与原 fixture 一起表驱动round-trip |
| 赋值对齐块少一列 | 修正 |

**变异测试验证**：分别让 `makeAuthoringDraft` 丢弃 `m_provenance`、丢弃 `m_forbidden`、把 `m_classification` 固定为 `Positive`，三次变异**全部被新测试独立捕获**；修复前的 fixture 三次全部漏过。

## 4. 决策项与后续处理

以下问题在评审时需要判断，不属于「唯一正确解」，因此当时只记录
而未改动。2026-07-23 的后续决策与落地状态分别记在各项末尾。

### 4.1 medium：`compileAuthoringDocument` 无工作量配额

`modules/annotation/source/annotation/authoring-compiler.cpp:194`

per-source 内层循环对绑定到该 source 的**每个** recognizer 无条件调用 `generateTemplateAsset()`。去重发生在**生成之后**（按内容哈希），而唯一的配额 `g_maximumCompiledTemplateBytes`（512 MiB）只累加**唯一模板的编码后字节**——模板全相同时永远不会触发。

构造触发：一张 8192×8192 纯色 PNG（约 35 KB）加 4096 个 `template_rect = [0, 0, 8192, 8192]` 的标注，全部通过解析校验，随后 4096 次全帧裁剪 + swizzle + PNG 编码，约 1.1 TB 像素处理，进程被占住数分钟到数小时且无取消点；因裁剪结果哈希相同，512 MiB 配额始终不触发。

**性质**：无界工作量/资源耗尽的加固缺口，不是正确性或内存安全缺陷；输出正确，峰值 RSS 约 800 MB。今天只能经库 API 触及，二者只有测试调用，`${PROJECT_NAME}_workbench_support` 未被链接进任何可执行文件，也不存在「打开项目目录」的摄入路径。**潜在而非在线。**

**为何不自行修**：两条候选路径（把去重键改为 `(sourceId, templateRect)` 在生成前查表；或按像素面积累加预算）都会改变编译器的公开行为契约与错误语义，属于设计决策。

**后续决策（2026-07-23，已实施）**：编译前按每个 source 的
fingerprint 面积及每个唯一 `(sourceId, templateRect)` 任务的面积累计
固定 256 Mi-pixel 工作预算，并在哈希、解码、裁剪和编码前 fail-fast。
模板生成改为直接遍历唯一任务，重复 recognizer 只复用已生成的内容
哈希；原有 512 MiB 唯一编码资产配额与哈希碰撞字节比较保持不变。
测试覆盖精确边界、超出一个像素和重复任务只计一次。

### 4.2 low：允许空页签名

`modules/annotation/source/annotation/catalog.cpp:487`

`PageSignature::create` 接受 `required` 与 `forbidden` 同时为空的页签名，这样的页在**零帧证据**下即成为候选。需澄清：这不违反设计文档 §3.4 的 load-time 校验清单（清单未要求页签名非空），也不等于零证据下发点击（授权仍需完整的同帧 `ActionDetection`）；多页场景下无锚点页恒为候选会把正确匹配退化成 `Ambiguous`（fail-closed）。真正的敞口是单候选场景。

**为何不自行修**：加一条新的校验规则会改变已锁定契约的行为，且需同步更新设计文档的校验清单。

**后续决策（2026-07-23，已实施）**：空签名不是「默认页」的合法
表达，也不得作为无证据匹配。`PageSignature::create` 现在拒绝 required
与 forbidden 同时为空；仅含 forbidden 的签名仍合法。契约已同步到
annotation 设计文档，并有正、负向测试钉住。

### 4.3 low：成员对齐规则跨提交不一致

`7625de6` 在特性序列中途把成员/赋值对齐变成强制项，但未附带 reformat pass，也没有自动化检查。结果是规则之后写的头文件全部合规、之前的全部不合规。

**这不是本分支特有问题**：`origin/master` 上 76 个成员块中已有 63 个未对齐；HEAD 上仍有 59 个未对齐块落在本分支从未触碰的既有文件里。仓库无 `.clang-format`，`scripts/fix_format.py` 只做字节级规范化，门禁抓不到。

**为何不自行修**：只改本分支的 6 个文件并不能消除模块内的混合风格，反而制造新的不一致。两条出路都需要你定：（a）单独一次全仓库对齐 pass 并给门禁加检查或加 `.clang-format` 的 `AlignConsecutiveDeclarations`；（b）明确声明该规则只适用于新写和被触碰的代码。

**后续决策（2026-07-23）**：先采用了 `clang-format 22.1.8` + 120 列的全仓
迁移，**随后回退**。原因是该方案超出了本条发现的范围：本条只针对成员与赋值
对齐，而全仓 clang-format 同时重写了 April2 换行约定，并把该约定从
`coding-standard.md` 中删除。

实测确认两者无法共存：用固定版 clang-format 对迁移前代码做还原实验，
120 列/DontAlign 在 40 文件样本上产生 5283 行差异，最接近的 88 列/BlockIndent
仍有 4166 行差异，且仅 5/40 文件能逐字节还原。April2 含判断性条款
（「短参数在可读时可共享一行」），任何 clang-format 配置都无法表达。

**最终落地**：April2 换行恢复为人工约定并写回 `coding-standard.md`；
`.clang-format` 删除；`scripts/check_cpp_format.py` 改写为纯标准库脚本，
只检查成员对齐与赋值对齐两条机械规则，默认只报告、`--fix` 为可选，
无法可靠识别的构造一律跳过而不猜测。门禁位置与调用方式保持不变。

### 4.4 建议但未执行的门禁变更

`scripts/check_safety.py` 的 `SOURCE_ROOTS = ("modules", "entry")` 不含 `tests`。项目把测试当生产代码（本报告多处按此标准判定），门禁却不看测试目录——§3.4 的违规正是因此长期无人发现。把 `"tests"` 加进去是一行改动，但会改变门禁范围、可能一次性暴露既有违规，属于策略决策。

**后续决策（2026-07-23，已实施）**：安全门禁现已扫描 `tests`，
扩展名匹配不区分大小写，并继续排除 vendored 目录。工具脚本测试覆盖
扫描范围、大小写扩展名、formatter 版本/路径/模式/批处理与失败传播；
`test-check-safety` 带 `CI` 标签，由 GitHub 各构建车道执行。

### 4.5 其余记录在案的低优先项

以下均已验证为真，但价值密度较低，未纳入本次评审修复，也未纳入
2026-07-23 的四项后续决策：

1. `pixels.cpp` 新增的前置几何守卫使搬迁来的逐行边界检查中 `*sourceEnd > source.size()` 不可达；对应测试用例名与实际覆盖的分支不符（实际覆盖的是 stride 守卫）。建议改名并补一个 stride 合法但缓冲区偏短的用例。
2. `tests/image/test-pixels.cpp:65` 用例名声称验证「使用帧灰度核」，但全仓库只有一个 `uf::bgra8ToGray8`，两次调用绑定到同一函数，将来分叉也发现不了。建议改名或删除。
3. `recognition-runtime.cpp` 的帧尺寸守卫与闭包哈希顺序检查无测试；帧尺寸守卫在生产路径可达。
4. `recognition.cpp` 有 6 个分支无测试，其中真正值得补的是「非 page_anchor 拒绝」与「完备性守卫」两个，其余四个是生产调用方到不了的纵深守卫。
5. `regression-runner.cpp` 的 `WgcSourceProvenance` 传播分支从未被执行（全部 fixture 都用 `ImportedSourceProvenance`）。
6. `recognition-runtime.cpp` 的 `pageRecognitionFailure` 与 `recognition.cpp` 的 `stopFailure` 是同一映射的逐字重复（约 27 行）。纯 DRY 清理——两份拷贝各自被测试钉住，单边修改会打挂自己那套测试而非静默通过。

## 5. 提交历史

### 5.1 `30665e0` 混入了一次未声明的抽取式重构

该提交在新增 authoring compiler 的同时，把 6 个提交前才落地的 `runtime-manifest.cpp` 削减了 612 行，将其私有的 TOML 读写 helper 迁到新的 `detail/canonical-toml.{hpp,cpp}` 并重命名。提交只有 subject 没有 body。

公允地说：这次抽取是该特性的**真实前置条件**（同提交的 `authoring-document.cpp` 在 7 处消费 `detail::CanonicalTomlReader`），且行为保持——`tests/annotation/test-runtime-manifest.cpp` 有意未改动，从而守住了这次搬迁。因此它落在「one minimal behavior change」的豁免边缘，而非明确违规。

### 5.2 `7625de6` 在特性序列中途引入强制风格规则

它比 `30665e0` 早 91 秒落地，把对齐变成强制项却未做迁移。详见 §4.3。

### 5.3 本次重组

按上述结论，14 个提交已重排合并为 5 个语义提交（image 重构 / annotation 后端 / workbench 持久化与编辑历史 / vision SAD / 文档与规范），另加本次评审产生的修复提交与本报告提交。重组后代码树与重组前逐字节一致，仅历史形状改变。

## 6. 覆盖说明

**已覆盖**：`origin/master..HEAD` 全部 14 个提交所触及的已提交文件的最终状态与 diff。维度包括项目规则合规、错误处理契约、逐子系统正确性（annotation 契约层、资产生成、authoring 编译器与 canonical TOML、识别运行时、回归执行器、workbench 持久化与 Win32 发布、image 重构、vision SAD、编辑历史）、安全与敌意输入、测试有效性、架构边界与构建接线、提交历史卫生、过度设计与重复、文档准确性。

**未覆盖 / 限制**：

- **未审查 vendored 代码**：`tests/external/doctest`、`modules/script/external/luau`、`modules/image/external/stb`。
- **clang-analysis / linux-analysis 车道未实跑**。本机为 Windows，只跑了 `x64-debug`。§3.5 修复了一处会打断该车道的写法，但不能保证该车道整体通过。**这是推送后需要在 CI 上确认的第一件事。**
- **Win32 发布路径的真实持久性未做掉电测试**。评审只做了 API 契约层面的审查（返回值检查、句柄泄漏、flush 与 rename 顺序、部分写循环、UTF-8/UTF-16 转换、长路径），未构造断电或文件系统故障注入。
- 本报告中所有「删除某守卫后全套测试仍通过」的论断，除 §3.2 与 §3.3 两处**已实测复现**外，其余来自验证阶段的报告。

**验证状态（修复后，本机 `x64-debug`）**：

```
python scripts/fix_format.py --check   OK (282 files)
python scripts/check_modules.py        OK (7 modules)
python scripts/check_safety.py         OK (146 files)
cmake --build --preset x64-debug       OK
ctest -L CI                            12/12 passed
```
