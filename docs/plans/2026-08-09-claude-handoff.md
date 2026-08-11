# Umbraflow upstream Runtime v2 / Operator handoff for Claude

更新时间：2026-08-09

## 1. 接管目标与硬边界

继续完成 Umbraflow 上游通用框架改造。唯一可写仓库/工作区是：

```text
E:\github\umbraflow-cpp-annotation-design
```

消费者项目仅可作为规范来源读取，严禁写入：

```text
E:\umbraflow-projects\uf-chaos
```

特别约束：

- 不做兼容性保留。不要增加旧 reader、alias、fallback、兼容 overload、双写或旧 CLI。
- 允许破坏性修改和删除无用代码。
- 保留通用可插拔 `ProjectPlugin` 边界，但不能把 Chaos 当作唯一消费者。
- 上游 fixture 可以有两个结构不同的假项目；真实双游戏 gate 必须保持 `EXTERNAL / NOT_RUN`，不能用 fixture 冒充。
- production RuntimeArtifact 不携带标注截图；截图只属于受 retention/privacy policy 约束的离线 Replay Bundle。
- 普通调用者只提交 tool + args，不得填写 mutability、effect、risk、binding、Receipt、model/frame/cycle 等权威字段。
- 目标是最少可信代码；不要引入 DI 容器、动态库 ABI、远程插件、消息总线、两阶段提交或通用 workflow engine。
- 不要 reset/clean/checkout 覆盖当前 dirty worktree；其中包含大量尚未提交的有效实现。

开始前完整阅读：

```text
E:\github\umbraflow-cpp-annotation-design\AGENTS.md
E:\github\umbraflow-cpp-annotation-design\CLAUDE.md
E:\github\umbraflow-cpp-annotation-design\.claude\skills\build-project\SKILL.md
E:\github\umbraflow-cpp-annotation-design\.claude\skills\post-change-validation\SKILL.md
```

## 2. 权威设计基线（只读）

项目侧最新设计是 v1.7。接管时的 exact SHA-256：

| 文件 | SHA-256 |
|---|---|
| `docs/architecture/umbraflow-game-automation-final-design.md` | `ee10aaa76dac787a3a9354ab47849e5048dfd0ca194c634d7ee55629d2a0fcf4` |
| `docs/architecture/spec-bundle.manifest.json` | `90496173f8a920e59659b7c4568c8d04abe7b4d3d209db493a59fc3c55226ed0` |
| `docs/architecture/uf-chaos-project-layer-design.md` | `65d444f73a6d682ca5a5b36b7b8393666da53c266892653f1d3141a99abc7bb3` |
| `docs/architecture/requirements-traceability.md` | `6864b06f180e4cd7851d1db127ae3cfb190827ae7c1413d2d3c4e56ef1d79cd6` |
| `docs/architecture/failure-and-recovery-audit.md` | `1fe1612a78ebb4c5506bf52c3e8a70a5285c165bce18ec679b724ec4d47a28a1` |

如果这些 hash 变化，先重读新版本并重新做需求差异检查，仍不得修改消费者仓库。

> 2026-08-10：bundle 已升到 1.8。上表是接管时的记录，保持原样。当前哈希见
> [ARCHITECTURE](../ARCHITECTURE.md) 与
> [runtime hardening authority](2026-08-09-runtime-hardening-rewrite.md)。
> 本条要求的差异检查已做：设计文档只做了原地版本号替换，本仓库依赖的七处契约面
> （disposition 五值、reduce 信封、I-13 与契约 15、两条重放门禁、Replay Bundle
> 闭包、authoring capability root、provenance 词表）字节与行号均未变动。项目层
> 文档的两处纠正不改变上游契约。

## 3. Git / worktree 状态

```text
branch: design/annotation-system-v2
base HEAD: 1b89760227e070fcf88f2778c312dce5cc9d87b4
base subject: docs: publish annotation runtime ownership contract
```

当前没有为这批实现创建 commit。工作树有大量修改、删除和未跟踪文件，这是预期状态。没有 `.codegraph/`，无需 CodeGraph。

此前并行 agent 已全部停止；没有共享写入者。最后两个 agent 被中断，因此下面明确标记的返工没有完成，不能相信先前的“完成”自报。

## 4. 已落地的主要改造

### 4.1 Runtime v2 / Task / Host

- Runtime v1 Page/Element/Hit/UFR、旧 task loader、旧确定性/RNG/回放入口已删除。
- RuntimeArtifact 只有一套 manifest + exact bytes/hash/path/asset closure 协议。
- `RuntimeArtifactHandle` 冻结 manifest/model/assets bytes；Host 不解释完整 TOML 语义。
- trusted Luau bootstrap parser 产生 frozen RuntimeModel binding。
- production Host 激活只接受 move-only `InstalledRuntimeArtifact`，不再接受任意文件路径。
- Annotation authoring 与 production activation 已分开。
- Phase 1 仍不公开 production click/key/drag/run；Receipt 只在私有 harness 验证。

关键文件：

```text
modules/task/source/task/runtime-model-file.hpp/.cpp
modules/task/source/task/task-host.hpp/.cpp
modules/task/source/task/task-context.hpp/.cpp
modules/task/runtime/*.luau
tests/task/test-runtime-v2-contract.cpp
```

### 4.2 Trace / Engine / CLI

- Trace 已改为 screenshot-free typed Audit Trace，不再冒充 Replay Bundle 或 Game Journal。
- 旧 JSON scanner/replay source/text API 已删除。
- Engine 使用 `TraceEventSpec`。
- 旧 `check/run/replay`、file-frame source 和相关测试已删除；CLI 只保留安全的 `explore/targets` 面。
- `tests/CMakeLists.txt` 预计注册 42 个 doctest contract case，加 `check-repository-surface` 共 43 个 contract gates。
- Trace schema hash 已集中为 `trace::k_traceSchemaHash`，并由 repository surface test 对 checked-in exact bytes 做校验。

### 4.3 Annotation workspace / publication

当前 Python 后端已经包含：

- immutable candidate revisions；
- human review、trusted replay intent/result、publication capability 分权；
- replay result 对 candidate/revision/runtime model/kind/corpus/policy 的绑定；
- frame + transition 两类 replay gate；
- publication/head predecessor CAS；
- publication/recovery cross-process lock；
- official Draft 2020-12 schema validation；
- loopback bearer token + Host/Origin 检查；
- immutable SQLite triggers；
- blob quota/GC 与 authoring/runtime asset namespace 检查；
- RuntimeArtifact + release handoff 输出。

关键文件：

```text
tools/annotate/contracts.py
tools/annotate/jcs.py
tools/annotate/publication.py
tools/annotate/safe_paths.py
tools/annotate/store.py
tools/annotate/trusted.py
tools/annotate/serve.py
tools/annotate/tests/test_backend.py
schema/umbraflow-annotation-workspace-v2.schema.json
```

当前 annotation tests 通过，但最后一个安全修复 agent 在独立复审前被中断。因此必须重新审计，不能仅凭测试宣布完成。重点复核第 6 节。

### 4.4 Operator Runtime / production installation

- 新增 `modules/operator` 和 SQLite production ledger。
- production RuntimeArtifact 使用 deployment release manifest、复制后复验和 installed-generation CAS。
- session pin 固定 RuntimeArtifact root + installed generation；旧 session 不随新安装热切换。
- session epoch、lease revision、fencing、snapshot token、idempotency、mutation chain、dispatch uncertainty/recovery 已有实现和 contract tests。
- authoring SQLite 与 production SQLite 没有 attach/跨库事务。
- 当前 Operator DB schema fingerprint 的计算值和嵌入值一致：

```text
d445c811b9469a58ff116df4763d4e7f1acd80b6a3392639d7eb257321916753
```

任何 DDL/trigger/index 修改后都必须重新计算，不能保留双 fingerprint。

> 2026-08-10：上面是接管时的记录，保持原样。W8 的 runtime artifact 回收往 DDL 里
> 加了 `runtime_publications` 表和 `runtime_state.active_runtime_artifact_root_hash`，
> fingerprint 现为
> `5738e6f98534efbdfc3114413de70c032b64e2cbaa84d4c152ec6cbb512120a4`，全树只出现
> 一次，在 `modules/operator/source/operator/ledger.cpp`。旧 Operator 数据库在 open
> 时被拒绝，不迁移而是重建。缘由见
> [next block](2026-08-10-next-block.md) 第三节与
> [review](../reviews/2026-08-10-runtime-hardening-review.md) 的 A-F8。

> 2026-08-11：上面两条同样保持原样，只补当前值。W3（`4b955de`）与 W2（`848e390`）
> 落地，新增 `project_observations`、`operation_plans`、`operation_steps`；同一个
> W2 commit 删掉了 `runtime_publications`——第三轮评审 R3-F2 证明没有任何测试能观察
> 到它，且代码不支持为它辩护的注释。现在共 20 张表，fingerprint 为
> `sha256:12f64bfff305c30c716fbd5bdc9934a17140dfe4e127b5bce2ec7a10ecd309e4`，仍
> 只在 `ledger.cpp` 出现一次，并且已提升为具名常量
> `k_exactSchemaV1Fingerprint`。「改 DDL 必须重算、不得保留双 fingerprint」这条
> 约束不变，两次落地都遵守了。

> 2026-08-11 当天稍晚：同样只补当前值。W4（`e64c143`、`25f57f9`）、W6（`93698b4`）、
> W7（`c23efd3`）相继落地，新增 `external_input_findings`、`ledger_events`、
> `agent_budgets`，现在共 23 张表，fingerprint 为
> `sha256:bda31e4b18a8096b28e5208f5988dea8658bea9d7917d78cd8655d4f581a8559`。
> W4 没有加表却也改了 fingerprint——`dispatches` 的 DDL 文本变了——这正是那条约束
> 想覆盖的情况。42 条 `REQUIRED_CORE` 需求全部实现完毕。

> 2026-08-11 更正（`07abc3e`）：上一段两处已过时。fingerprint 又动了一次，现为
> `sha256:be80aca714a29c976f53d4bdfe39571975a839027cc3efd15822db8a7df3e7b1`，
> 仍是 23 张表——`07abc3e` 把八个 DDL 列名统一成 `controlled_target_id`，只改
> DDL 文本不改表名，正好又是那条约束覆盖的情况。另外「42 条需求全部实现完毕」这句
> 本身没错，但当时被读成「全部关闭」：`a07` 的验收有两条子句，`contract-agent-a07`
> 只证明了第二条（在途 dispatch 被明确报告），第一条（takeover 与 Host delivery
> 同一线性化序列）无任何实现——takeover 事务与 `TaskHost` 的 fence 之间，生产和测试
> 里都没有调用边。**以行为 gate 关闭的是 39 条，不是 40 条**，`a07` 重新打开，详见
> [next block](2026-08-10-next-block.md) §2。
>
> 2026-08-11 再更正：fingerprint 又动了一次，现为
> `sha256:500c07b10eb263c0f2d6001e0a8b9a90ddd2afd951130cef71f5dbbfbd66085a`，
> 仍是 23 张表——`journal_events` 和 `project_state` 的四个列改用了
> `$defs.JournalEvent`/`$defs.ProjectState` 已经在用的成员名，此前建好的
> 数据库打开时会被拒绝并删除，不做迁移。详见
> [journal record binding](2026-08-11-journal-record-binding.md)。
>
> 2026-08-11 三度更正（`bed456f`）：`a07` 那段的读法本身是错的。冻结包那一行把两个
> 后果都放在验收里——「takeover 返回后旧 fence 不可开始新 dispatch；在途 dispatch
> 被明确报告」——不是一个在需求、一个在验收。第一条子句是 `reserveDispatch` 的
> live-lease 判断 `requireLiveLease`，在重新打开之前就已经跑在 `takeoverLease`
> 提交所在的同一个 `BEGIN IMMEDIATE` 序列化里；缺的不是调用边，是一个真正跑过这条
> schedule 的测试——没有用例在 takeover 之后拿被替换的 lease 去尝试一次 reservation。
> `contract-agent-a07` 被扩展（不是新增用例）去跑这条 schedule，两个子句现在都能被
> mutation 证伪。**42 条需求全部由行为 gate 关闭**，gate 数不变，仍是 40 个
> `contract-*` 加 19 个 `schema-*`。详见
> [next block](2026-08-10-next-block.md) §2，那里是当前记录；上面两段关于 `a07` 的
> 更正留作误读及其更正的记录，不代表需求现状。

### 4.5 ProjectPlugin

- 只支持 startup-time registry `(plugin_id, project_registration_hash)` exact lookup，无 latest/fallback。
- 固定五函数：`derive/plan/next_step/reconcile/reduce`。
- exact plugin bytes 必须匹配 registration 中 `plugin_hash`。
- project artifact roots 按 name 做严格 missing/extra/duplicate/hash closure 校验。
- fresh quota-bound pure VM 只暴露 frozen `artifact.read(root_name)`；没有路径、loader、Host、DB、clock、RNG、native registry 或 FFI。
- 每次调用新建 VM；调用方修改原 blob 不影响已注册 handle。
- 限制：64 roots、单 blob 4 MiB、总计 16 MiB、输入/输出 1 MiB、错误文本 4 KiB。

关键文件：

```text
modules/operator/source/operator/project-plugin.hpp/.cpp
modules/operator/source/operator/manifest.hpp/.cpp
modules/script/source/script/pure-data-program.hpp
modules/script/source/script/ffi/pure-data-program.cpp
tests/operator/test-project-plugin-contract.cpp
```

ProjectRegistration schema 已在本轮额外收紧：

- `plugin_id`、`baseline_event_type` 必须是 lowercase ASCII namespaced name；
- artifact root name 使用相同 segment 规则，但允许单段；
- `../content` 一类路径拼写被拒绝。

对应 C++ 攻击测试已加到 `tests/operator/test-manifest.cpp`，尚未 build/run。

### 4.6 Journal schema ownership（只完成一半）

新增：

```text
modules/operator/source/operator/journal-entry.hpp/.cpp
```

`ProjectJournalSchemaOwner` 绑定 exact registration，按 event type 验证 payload schema，并验证固定 provenance schema；`ValidatedJournalEntryData` 私有构造，调用方不能自报 `payload_schema_hash`。

这一层的 schema-label forgery 已关闭，但 reducer input 仍未关闭，见下一节 P0。

## 5. 当前检查结果

接管前最后一次执行结果：

| 检查 | 结果 |
|---|---|
| `python -m unittest tools.annotate.tests.test_backend` | PASS，29 tests，1 skipped，约 17 秒 |
| `python scripts/check_modules.py` | PASS，11 modules |
| `python scripts/check_safety.py` | PASS，258 files |
| 所有 `schema/*.schema.json` Draft 2020-12 meta-validation | PASS |
| `git diff --check` | PASS |
| `python tests/test-runtime-surface.py` | FAIL，仅因 Annotation workspace schema authority hash 尚未同步 |
| `python scripts/check_cpp_format.py` | FAIL，多处 alignment；最后统一 `--fix` |
| configure/build/CTest | **从未运行** |
| 真实双游戏 gate | `EXTERNAL / NOT_RUN` |

当前 Annotation workspace schema exact SHA-256：

```text
4f19ecf38fc92c6f505ee40788330c567d14c4f9324b8e3e7a51e193b92ec6c9
```

`modules/operator/source/operator/runtime-installation.cpp` 仍嵌入旧值 `d3b67...`，所以 surface test 正确失败。完成 annotation 审计、确认 schema 不再变化后，只保留新值。

> 2026-08-10：上面两个值都是接管时的记录，保持原样。schema 后续还改过，当前
> checked-in 字节的 SHA-256 是
> `a6fc31b5e0ee49f5368d66fae3f2abf38e0e58f57d799e3d2cd8da583f508a29`，
> `k_annotationWorkspaceSchemaHash` 已同步，`tests/test-runtime-surface.py` 盯住
> 两者。W5 的 Replay Bundle 只改了 Python 侧 workspace SQLite schema root hash
> （现为 `72fa0c39964397921007665e2f4f3f7936bd46f476a3adf589d32bd59ce9d873`，
> 新增四张表），C++ 不 pin 这个值。

## 6. 必须先修的已知问题

### P0-1：Journal events 与 reducer input 仍可分叉

以下 caller-controlled 字段仍存在：

```text
ProjectInstanceBaseline.reducerInput
ReconciliationCommit.reducerInput
```

位置：

```text
modules/operator/source/operator/ledger.hpp
modules/operator/source/operator/ledger.cpp
tests/operator/test-ledger.cpp
tests/operator/test-control-contract.cpp
```

问题：调用方可以让 Journal 写入事件 A，却向 reducer 提交输入 B；当前 reconciliation 的 `plugin.reduce()` 还发生在 SQLite transaction 之前。这违反“Journal prefix 是 materialized ProjectState 唯一来源”。

必须破坏性修复：

1. 删除两个 `reducerInput` 字段，不留 overload/alias。
2. Operator 从 `ValidatedJournalEntryData` 和数据库中的当前 canonical ProjectState 唯一构造固定 generic reduce envelope。
3. 建议 exact JCS 形状只包含按键排序的 `journal_events` 与 `prior_project_state`；每个 event 嵌入 namespaced type、payload、provenance。不要让调用方传整段 envelope。
4. baseline 使用 `prior_project_state:null` 和实际 baseline entry。
5. reconciliation 在 `BEGIN IMMEDIATE` 后读取并核对 current state/registration，再调用 pinned reducer；Journal insert、state CAS、reconciliation、Operation transition 同一事务提交。
6. `Rejected`/`Ambiguous` 不得借 Journal/state commit 写已发生效果；`Continue/Confirmed/Diverged` 按 v1.7 语义约束。
7. 增加 A-event/B-state 分叉攻击测试和失败后无写入测试。

### P0-2：`CommandRequest.mutating` 仍由调用方提供

当前字段：

```text
modules/operator/source/operator/ledger.hpp: CommandRequest::mutating
```

`createOrLoadOperation()` 用这个 bool 决定是否取得 mutation chain。调用方可以把 mutating tool 降级为 read-only，违反 Tool Catalog 唯一权威。

必须破坏性修复：

1. 删除 caller bool，不留兼容 API。
2. 建立最小 `ProjectToolCatalogSchemaOwner`/`ValidatedToolInvocation` authority，绑定 exact `VerifiedProjectRegistration` 和 `tool_catalog_hash`。
3. trusted catalog validator 验证 tool name/version/args 的完整 schema，并从 descriptor 返回真实 mutability；普通 request 只提交 tool + exact canonical args。
4. `createOrLoadOperation()` 核对 invocation registration root 与 session pin 后使用 authority-owned mutability。
5. command fingerprint 覆盖 tool/version/exact args；mutability 不由 caller 参与。
6. 增加 caller downgrade、cross-registration、wrong version/schema 攻击测试。

### P0-3：Annotation schema authority hash 漂移

在 Annotation schema最终定稿后，把：

```text
modules/operator/source/operator/runtime-installation.cpp
```

中的 `k_annotationWorkspaceSchemaHash` 更新为 checked-in exact bytes hash。`tests/test-runtime-surface.py` 已自动阻止再次漂移。

### P1-1：Annotation 安全修复尚未独立复审

虽然 Python tests 已通过，仍逐项人工/agent 复核：

- replay result 是否精确绑定 candidate id/revision/runtime_model_hash/kind/corpus/policy；publication 不得回填身份；
- capability/replay policy 是否只能由 purpose-bound file descriptor reader 铸造，调用方不能构造或修改；
- human-review/replay-runner/publication root 是否 exact closure 且相互隔离；
- publish/recover 是否共享同一个跨进程锁，不能在 DB commit 前删除 staged root；
- RuntimeArtifact 和 RuntimeModel 是否使用官方 Draft 2020-12 validator，不能回退到不完整 custom validator；
- evidence blobs 与 deployable runtime assets 是否 namespace-disjoint，同 hash 截图不能重标成 runtime asset；
- loopback API 是否强制 bearer、单一 Host、允许的 Origin，拒绝重复 header；
- failed upload/orphan blob 是否可回收，单请求/总量是否有 quota；
- immutable DB rows 是否有 UPDATE/DELETE triggers；
- replay attestation 是否恰好一条 frame + 一条 transition；
- symlink/junction/reparse/check-open-delete TOCTOU 攻击测试是否真实覆盖 Windows。

### P1-2：RuntimeArtifact loader / installer 的 handle-based confinement 需复核

`modules/task/source/task/runtime-model-file.cpp` 和 `modules/operator/source/operator/runtime-installation.cpp` 当前主要使用 `std::filesystem::symlink_status/canonical` 后再 `ifstream/ofstream`。冻结 bytes + exact hash 能阻止大部分身份替换，但 v1.7 明确要求 confinement-open 并拒绝 Windows junction/reparse point。

必须判断并测试 hostile mutable directory 下的 check/open race；若当前实现不能证明安全，应使用平台 handle-based no-reparse open，不能仅增加一次重复 `canonical()`。不要把 authoring root 或 handoff root 假设为可信而弱化已写入规范的不变量。

### P1-3：C++ 尚未编译

目前所有 C++ API 合并仅经过静态扫描。特别关注：

- 新 `journal-entry` source 是否被模块生成器纳入；
- `ProjectPluginRegistrar::registerPlugin` 新 artifact 参数是否所有 caller 都适配；
- `std::format`/C++23、Luau C API stack discipline、SQLite DDL fingerprint；
- 删除旧文件后 CMake 是否还有引用；
- Windows/unsupported CLI 两套 target 是否均可编译。

## 7. 推荐完成顺序

1. 重新读取第 1、2 节文件并确认消费者规范 hash 未变化。
2. 保存 `git status --short`，不要 reset/clean。
3. 先修 P0-1 Journal/reducer 单一来源。
4. 再修 P0-2 Tool Catalog authority，删除 caller mutability。
5. 独立复审 Annotation，修完后同步 P0-3 schema hash。
6. 复核 RuntimeArtifact path/reparse/TOCTOU 边界。
7. 如果 Operator DDL 改变，重新计算并替换唯一 DB fingerprint。
8. 运行 Python/schema/static tests。
9. 最后统一运行 `python scripts/check_cpp_format.py --fix`，避免中途机械格式化干扰逻辑 diff。
10. 使用项目 build skill 做完整 configure/build/CTest。
11. 派两个独立 reviewer：
    - state/persistence/recovery/production CAS；
    - plugin/capability/annotation/Host boundary。
12. 两者 PASS 后再更新上游 `CONTEXT.md`、`docs/ARCHITECTURE.md`、`docs/TODO.md`、`docs/WORKLIST.md` 和 migration report。不得修改项目侧文档。

## 8. 验收命令

### 8.1 快速静态门禁

```powershell
python -m unittest tools.annotate.tests.test_backend
python scripts/fix_format.py --check
python scripts/check_cpp_format.py
python scripts/check_modules.py
python scripts/check_safety.py
python tests/test-runtime-surface.py
git diff --check
```

全部 JSON Schema meta-validation：

```powershell
@'
import json
from pathlib import Path
from jsonschema import Draft202012Validator
for path in sorted(Path('schema').glob('*.schema.json')):
    Draft202012Validator.check_schema(json.loads(path.read_text(encoding='utf-8')))
print('all schemas: PASS')
'@ | python -
```

### 8.2 Operator DB schema fingerprint

DDL 改动后运行：

```powershell
@'
import re, sqlite3, hashlib, pathlib
p = pathlib.Path('modules/operator/source/operator/ledger.cpp').read_text(encoding='utf-8')
sql = re.search(
    r'UF_TRY\(execute\(\s*database,\s*R"sql\((.*?)\)sql"\s*\)\);',
    p,
    re.S,
).group(1)
c = sqlite3.connect(':memory:')
c.executescript(sql)
rows = c.execute(
    "SELECT type,name,tbl_name,coalesce(sql,'') FROM sqlite_schema "
    "WHERE name NOT LIKE 'sqlite_%' ORDER BY type,name"
).fetchall()
encoded = ''.join(
    f'{len(value.encode())}:{value}' for row in rows for value in row
).encode()
print(hashlib.sha256(encoded).hexdigest())
'@ | python -
```

### 8.3 完整 build gate

必须按 `.claude/skills/build-project/SKILL.md` 使用项目环境，最终完整命令是：

```powershell
cmd /c "call .claude\skills\build-project\script\windows\build-env.bat && pwsh -NoProfile -File scripts\ci-local.ps1"
```

完成后还要检查：

```powershell
ctest --test-dir build -N
```

实际 build directory 以 skill/script 输出为准。`ctest -N` 必须能列出全部 43 个 contract gates；不要只数普通 test executable。

> 2026-08-10：43 仍然是 42 个需求 gate 加 `check-repository-surface`，这条保持
> 原样。P-05 的可消费契约套件落地后，`ctest -N` 另外列出
> `conformance-umbraflow` 和 `conformance-arcana`（label `CONFORMANCE`），
> 所以列表长于 43 不是回归。见 [next block](2026-08-10-next-block.md) 第五节。

> 2026-08-11：43 这个数字已经不能再当计数用了，上面两条保持原样。W10 把 gate 拆成
> `contract-*`（行为）和 `schema-*`（只读 schema 文件）两族，一个需求可以各持一个，
> 所以 42 个需求现在带 52 个 gate：33 个 `contract-*`、19 个 `schema-*`。权威是
> `tests/CMakeLists.txt` 的 `UF_REQUIRED_DOCTEST_CONTRACTS`，人读的映射在
> [migration report](2026-08-09-runtime-migration-report.md)。验收要看的是「每个
> `REQUIRED_CORE` 需求都有行为 gate」，不是某个总数。

## 9. 最终交付口径

只有以下条件都满足才能宣称上游改造完成：

- P0/P1 已关闭且没有兼容入口；
- static/schema/format/full CI 全绿；
- 43 个 contract gates 可见并执行；
- 两个独立 reviewer PASS；
- project side 零写入；
- production mutation 仍关闭，直到真实第二游戏和 Chaos 都通过同一外部门禁；
- 真实双游戏状态诚实报告为 `EXTERNAL / NOT_RUN`，除非确实在两个消费者仓库执行过。
