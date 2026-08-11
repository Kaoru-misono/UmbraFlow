# Cross-repository drift audit: framework vs uf-chaos, against the frozen v1.9 bundle

Status: report only. Nothing here is fixed, and this document changes no schema,
no code and no other document.
Date: 2026-08-11
Framework HEAD audited: `c23efd3` (`design/annotation-system-v2`)
Consumer HEAD audited: `7a35568` (`master`, no remote)
Bundle: v1.9, root `c4760bb59e7df28e13a676446a4cfbb4a62b067741420ecf13f4b939bfb6a966`

Both repositories were read-only throughout. No git command that writes was run
in either tree.

Locators are `path:line`. Bundle paths are relative to
`E:\umbraflow-projects\uf-chaos\docs\architecture\`; framework paths are relative
to the repository root.

---

## 0. The pin: VERIFIED, byte for byte

All four bundle documents on disk hash exactly to the values pinned in
[the hardening rewrite](2026-08-09-runtime-hardening-rewrite.md):14-18.

| Document | SHA-256 on disk | Bytes | Pin |
|---|---|---|---|
| `failure-and-recovery-audit.md` | `aad291c9…b00e55` | 19,697 | match |
| `requirements-traceability.md` | `2b725e81…24196c` | 13,383 | match |
| `uf-chaos-project-layer-design.md` | `c2e920a1…f110687` | 29,857 | match |
| `umbraflow-game-automation-final-design.md` | `3499e875…5ee0b44` | 81,932 | match |

The bundle root is `sha256(spec-bundle.manifest.json)` — computed
`c4760bb5…bfb6a966`, matching the pin. Every `byte_size` in the manifest matches
the file on disk. Verified independently twice, by two readers.

All five files carry mtime `2026-08-09 17:28:59`, so nothing touched them during
this audit even though another agent was writing elsewhere in that repository.

**The pin holds. Everything below rests on a verified arbiter.**

---

## 1. The finding that governs the others

Before the ranked list, one clause changes how every bundle disagreement in this
report must be read.

**`umbraflow-game-automation-final-design.md`:17-18**

> 若这些文件发生冲突，说明规范本身不完整，必须先共同升版并消除冲突，
> **不能由实现者自行选择其一。**

*"If these files conflict, the specification itself is incomplete; they must
first be co-versioned together and the conflict eliminated. **The implementer may
not choose one of them.**"*

The bundle anticipated its own contradictions and specified the remedy: re-version
the bundle. It explicitly forbids the mechanism the framework used instead —
[the hardening rewrite](2026-08-09-runtime-hardening-rewrite.md)'s four
"executable conformance resolutions", each of which picks one side of a bundle
disagreement and freezes that choice upstream.

This does not make the framework's individual choices wrong. B-1 and B-2 below
argue two of them are the *better* reading. It makes the **procedure** wrong, and
that matters more than any single choice, because a resolution recorded only
upstream is invisible to the consumer, cannot be reviewed by whoever owns the
bundle, and silently becomes precedent — as it already has: four resolutions have
grown into at least nine documented departures, and three of those are attributed
to the bundle in language the bundle does not support (see F-12).

**The correction is not to unwind the resolutions.** It is to issue bundle v2.0
carrying them, so that the arbiter and the implementation agree in one place
rather than in two documents that only one side can read. Until then, every
"executable conformance resolution" is a fork of the specification held by the
implementer.

---

## 2. Ranked findings

Ranked by consequence: what stops, misdirects, or silently weakens real work
first.

### F-1 — CRITICAL. The arbiter is uncommitted, partly untracked, has no backup, and its pin is enforced by nothing

Two independent failures that compound.

**(a) The bundle exists in exactly one place.** In `E:\umbraflow-projects\uf-chaos`:

- `git ls-files docs/architecture/` returns **three** files.
  `uf-chaos-project-layer-design.md` and `spec-bundle.manifest.json` are
  **untracked** — and not gitignored (`git check-ignore` returns nothing).
- The three tracked files are all **modified** relative to `HEAD`. The committed
  main design declares `版本：1.4` (HEAD line 4); the working-tree copy that
  hashes to the pin declares `版本：1.9` (line 5). The diff is 607 insertions /
  551 deletions and survives `--ignore-all-space` at 605/549 — real content, not
  line endings. (The tree is CRLF, HEAD is LF; that accounts for 2 lines.)
- `git remote -v` is empty. `git stash list` is empty.

One `git checkout -- docs/architecture/`, `git stash`, or `git clean -fd` in
uf-chaos reverts the arbiter to v1.4 and destroys v1.9 irrecoverably. The
framework's pin would then fail against a file nobody could reconstruct, and
`2026-08-09-runtime-hardening-rewrite.md`:20 — "If any byte differs,
implementation stops" — would halt implementation permanently.

**(b) Nothing checks the pin.** VERIFIED: the root hash `c4760bb5…` appears in
exactly eight files, **all of them prose documents under `docs/`**. Zero hits in
`scripts/`, `cmake/`, `tests/`, `modules/`, or any `CMakeLists.txt`. No gate
hashes the bundle; no CI step reads that directory; `scripts/ci-local.ps1` has no
knowledge of it. The sentence "If any byte differs, implementation stops" is
enforced by a human remembering to check.

This is the [checks that cannot fail](../pitfalls/checks-that-cannot-fail.md)
family applied to the arbiter itself: a name promises that the bundle is pinned,
and nothing verifies the promise. The re-hash performed for this report appears to
be only the second time anyone has actually done it.

**Which side is wrong.** Neither design. This is an operational gap, half on each
side, and it is the highest-value item in this report.

**Correction.** Commit the five bundle files in uf-chaos (consumer-side, not
ours). Upstream, add a check that re-hashes the bundle when the path is present
and *skips loudly* when it is not — a check that silently passes when the
directory is missing would be a ninth instance of the pitfall above.

**Incidental, and sharper than it looks.** uf-chaos's framework integration — root
`CMakeLists.txt`, `contract/CMakeLists.txt`, `contract/provider.cpp` — was
**untracked at the start of this audit and committed during it**, as `317d05f`
"feat: run the framework's contract suite against this project". The bundle it
ultimately answers to was not committed in the same change and remains in the
state described above. The consumer now has committed code binding it to a
framework contract, resting on an authority that exists only in a working tree.

---

### F-2 — The freeze that stops project work is stale, and the cause is checkout identity, not framework change

**What the consumer says.** `E:\umbraflow-projects\uf-chaos\2026-08-09-claude-handoff.md`
(untracked; header line 3 reads `Date: 2026-08-10 (Asia/Tokyo)`, a day later than
its filename):

- `:299-304` — "CodeGraph confirms upstream still has no Operator Core, no runtime
  `ProjectPlugin`, no journal and no Runtime v2 surface layer, so a C++
  implementation now could only be written against an invented boundary."
- `:368-371` — "Project-layer state work is frozen here. … upstream is the only
  critical path, and more project-side plumbing only widens the rework surface."
- `:373-374` — "Frozen: no new state, journal or reducer code in
  `modules/project_state/`."
- `:295` — the unblocking condition: "do not open C3 mutation until Runtime v2 +
  Operator + ProjectPlugin contracts exist and pass their upstream gates."

Restated in consumer code at `modules/project_state/manifest.txt`:13,
`reducer.py`:18, `model.py`:20.

**Why the obvious reading is wrong.** The document names its audit target at
`:282`: `E:\github\umbraflow-cpp`. VERIFIED — that checkout exists and genuinely
has **no** `modules/operator`; its `modules/` holds `controller core domain engine
image ocr script task trace vision` and nothing else. **The freeze was correct
about the tree it audited.**

The framework work lives in `E:\github\umbraflow-cpp-annotation-design`, which has
`modules/operator/` (including `journal-entry.hpp`, `ProjectPluginHandle`,
`ProjectPluginFunction::{Plan,NextStep,Reduce}`), `contract-suite/`, and
`cmake/operator-contract-suite.cmake`. All four things `:301` calls missing exist
there.

So the freeze is stale because the consumer is now pointed at a **different
checkout** — by its own `CMakeLists.txt`:13,
`set(UF_FRAMEWORK_ROOT "E:/github/umbraflow-cpp-annotation-design" …)`. No reading
of the framework's commits could have detected this; only comparing the audited
path against the built path does.

**It is being overtaken in real time.** `contract/provider.cpp` was a 4-line stub
early in this audit and 1,146 lines by its end (mtime 20:40), using `ToolSurface`
(`:220`, `:819`, `:947`, `:954`) — a type that did not exist before `93698b4`
tonight. Project-side work against the framework is happening while the document
forbidding it still stands.

**Which side is wrong.** The consumer document, on its conclusion. Its method was
sound. **Correction:** rewrite §7 and §11 to name the checkout audited and the
commit it was true of, then lift the freeze. Do not simply delete `:301` — it was
true of `E:\github\umbraflow-cpp` and still is.

---

### F-3 — `ToolDescriptor` lost six of its nine members, and four inventions exist to replace the checks it carried

The largest capability gap found, and the root of three other findings.

**Bundle** (`umbraflow-game-automation-final-design.md`:623-633) defines
`ToolDescriptor` with nine member groups:

```
name + version / input_schema + output_schema / mutability / idempotency
required_capabilities[]
effect_bounds[{namespaced_type, maximum_risk, scope_kind, payload_schema_hash}]
ui_action_bounds[] / workflow_limits / timeout_policy
```

`:700` makes enforcement mandatory: "Operator 拒绝未知 effect type、**超过
descriptor 风险上界**、scope 不匹配或 payload schema hash 不一致的 proposal，也拒绝
超出 `ui_action_bounds` 的 action." `:639` fixes the role: "**Descriptor 只声明静态
上界**."

**Framework.** `ToolDescriptor` appears in **no schema file**. The C++ struct is
three members — `modules/operator/source/operator/tool-invocation.hpp`:52-64,
`{toolVersion, mutability, surface}`. `effect_bounds`, `ui_action_bounds` and
`required_capabilities` return **zero hits** across `schema/`, `modules/`,
`tests/` and `contract-suite/`.

**The four substitutes.** Each exists because the descriptor does not:

1. `k_workflowCeiling{64,64,256,64,600000}` — a hardcoded **global** ceiling
   (`2026-08-10-w2-effective-plan.md`:267-273) replacing the bundle's **per-tool,
   project-declared** `workflow_limits`, which is covered by `tool_catalog_hash`
   and therefore by `project_registration_hash`.
2. `maximum_mutations` — an invented `AgentBudget` axis matching no bundle axis
   (see B-8).
3. `ToolSurface` — a boolean-ish marking on a project-owned catalog, standing in
   for the bundle's capability-non-existence requirement (see F-12, item 3).
4. R4's ruling that containment is "attribution, not prevention"
   (`2026-08-10-w2-effective-plan.md`:152) — presented as an unavoidable property
   rather than as the consequence of dropping `effect_bounds[].maximum_risk`.

**Which side is wrong.** The framework, and not by oversight — by an accumulation
of local decisions each of which looked reasonable. **Correction:** restore
`ToolDescriptor`'s bound-carrying members. It removes the need for all four
inventions and answers the P-03 question the framework recorded as unanswerable.

---

### F-4 — The takeover fence is never adopted in production, so invariant I-02 is not delivered — while `A-07` is counted closed

**Bundle.** `umbraflow-game-automation-final-design.md`:226-227:

> Host 在同一 mutex/临界区内检查 active lease/high-water fence、消费 Receipt 并调用
> 原生输入。takeover 安装更高 high-water 也走同一临界区。**这一个函数就是安全线性化点。**

`:216-217` — "takeover 返回后，任何旧 fencing 的新 dispatch 都不可能开始."
Restated as invariant I-02 (`failure-and-recovery-audit.md`:30) and as acceptance
`A-07` (`requirements-traceability.md`:116).

**Framework.** `2026-08-10-w4-delivery-join.md`:111-118 relocates the
linearization point to SQLite commit order — "those five are totally ordered by
commit order, **and that order is the linearization**. Operation 2 is not a
database operation and must not become one." And `:924-928` concedes:
"`adoptControlFence` is private and reachable only from the test harness, so
**nothing in production ever adopts a fence**."

Between `takeoverLease` returning and `adoptControlFence` running, a dispatch
under the old fence can still begin — exactly what I-02 forbids. W4 closes `a07`
against its second clause only, stating at `:422-424` that "the takeover does not
prevent the in-flight effect, it prevents the ledger from ever claiming the effect
did not happen." That is a real property; it is not I-02.

**Corroborating.** `TaskHost::deliver` is itself `private`
(`modules/task/source/task/task-host.hpp`:266-270; `public:` begins `:285`), with
`friend struct TaskHostTestAccess;` at `:193` its only friend. Production has no
route to the delivery entry point at all, so the bundle's "one function that is
the safety linearization point" has no production caller.

**Which side is wrong.** The framework, on the conformance claim rather than on
the design. **Correction:** either deliver the Host-side fence install on the
production path, or reopen `A-07` and record I-02 as not yet met. A requirement
counted closed against half its acceptance text is worse than one left open.

> **Applied 2026-08-11 (`07abc3e`): the second branch, and this finding was
> right.** `a07` is reopened and the invariant is recorded as unmet in
> [the next block](2026-08-10-next-block.md) §2, which also states the two ways
> it closes and that choosing between them belongs to an owner rather than to a
> document. The requirement count moved with it: **39 of 42 closed by a
> behavioural gate, not 40**, corrected in `docs/TODO.md`, `docs/INDEX.md`,
> `docs/plans/README.md`, the migration report's `A-07` row and the W4
> specification. Three details verified while applying it, each stronger than
> stated above: the disjointness is total — there is **no call edge** between
> `takeoverLease` and `TaskHost::adoptControlFence` in production *or in test*,
> and every one of the fence's 16 call sites passes a literal or a
> fixture-construction lease; the bridging API
> `operator::controlFence(ControlLease const&)` has **zero production callers**,
> its two test callers being `observation-fixture.hpp`:774 and
> `test-control-contract.cpp`:403, the latter fabricating the moved fence by
> hand-incrementing `fencingToken` rather than reading a takeover; and
> `mintClickReceipt` now refuses at fence 0
> (`modules/task/source/task/task-host.cpp`:578), which is a *second*
> independent reason production cannot dispatch and which post-dates the bundle
> text quoted above. F-4 remains the finding; this note only records that it was
> acted on rather than accepted.

---

### F-5 — The consumer's whole Luau task layer targets a host surface the framework bans in CI

**Consumer.** `ctx:` is the only host receiver in the repository — VERIFIED by
extracting every `receiver:method(` pair, not by searching for `ctx`. Exactly
**15 distinct verbs across 129 call sites in 9 `.luau` files**:

`cycle_open` (37 uses, `map\collect-probe-based.luau`:161), `cycle_close` (61,
`:163`), `cycle_read` (2, `tasks\daily.luau`:442), `cycle_read_lines` (2,
`daily.luau`:994), `cycle_match` (6, `map\collect.luau`:855), `cycle_census_grid`
(2, `tasks\map-collect.luau`:452), `cycle_scroll` (1, `daily.luau`:2924),
`cycle_move_pointer` (1, `daily.luau`:2921), `key` (4, `daily.luau`:1015), `wait`
(2, `daily.luau`:280), `deadline` (2, `daily.luau`:593), `try` (3,
`daily.luau`:3024), `template_load` (2, `map\collect.luau`:1236), `project_read`
(6, `daily.luau`:191), `project_write` (4, `daily.luau`:3099).

**Framework.** `ctx` is not merely absent — it is forbidden and CI-enforced.
`tests/test-runtime-surface.py`:69 defines `FORBIDDEN_BUSINESS_GLOBALS`
containing `ctx`, `key`, `move_pointer`, `click`, `action`, `input`, `press`,
`model`, `observe`, gated as `contract-repository-surface` under `ctest -L CI`.
The current surface is seven `explore_*` verbs at
`modules/task/source/task/ffi/uf-tables.cpp`:623-629.

**Which side is wrong.** The consumer. **Correction:** rewrite against the Explore
surface and the Operator tool path. Two secondary hits in the same family:
`daily.luau`:191 reads `encounters/runtime/encounters.ufr` (`.ufr` is an enforced
retirement), and `docs/encounters/调研-recognition.md`:109 documents
`resolve_page(ctx, ticket, page)`, a retired runtime symbol.

**Framework side of the same drift.** `ctx:` survives in framework *comments* at
`modules/task/source/task/task-context.hpp`:39,43,48 and
`modules/script/source/script/ffi/sandbox.cpp`:218, describing deleted verbs.

---

### F-6 — `page-model.toml` is not one schema version behind; it is a different document

**Consumer.** `page-model.toml`:1 — `schema = "umbraflow-project/l2-v2"`. 5,926
lines. Tables: `[[element]] [[page]] [[screen]] [[edge]] [[appearance]]
[[expect]] [[reference]]`.

**Framework.** The root key is `schema_version` and must equal 2 —
`modules/task/runtime/model.luau`:492. The accepted root key set is closed by
`only()` at `:481-491`: `schema_version, base_resolution, base_dpi, ui_targets,
locators, readers, bindings, surfaces, transitions`. A root key named `schema` is
not in that set, so the consumer's **first line** fails before any content is
read. Retirement recorded at framework `CONTEXT.md`:363-366 — replaced by
`umbraflow-runtime/v2` in `8af22bc`, no read path for the old id.

Only `base_resolution` and `base_dpi` survive. Every table name differs.

**Which side is wrong.** The consumer — though note the bundle never mentions
`l2-v2` at all, so this identifier's authority was always framework-side.
**Correction:** a rewrite, not a migration. There is no field-level
correspondence to migrate through.

---

### F-7 — `canonical_json_dumps` is not JCS, and the consumer has three variants that disagree with each other

**Arbiter.** `umbraflow-game-automation-final-design.md`:259 — "canonical JSON
精确定义为 RFC 8785 JCS 的 UTF-8 bytes：… 不加 BOM 或尾随换行."

**Consumer — three implementations, all plain `json.dumps`:**

- `modules/content/source_ledger.py`:47 — `json.dumps(value, ensure_ascii=False,
  sort_keys=True, separators=(",", ":"), allow_nan=False)`
- `modules/content/artifact.py`:24 — same, then `return (text + "\n").encode("utf-8")`.
  The trailing newline is **directly forbidden**, and means the same value hashes
  differently here than in `source_ledger.py`.
- `modules/content/event_graph.py`:1329 — same **minus `allow_nan=False`**, so it
  can emit bare `NaN`/`Infinity`. Feeds `graph_digest` (`:494`) and
  `document_digest` (`:634`).

Three further gaps beyond the newline: `sort_keys=True` orders by Unicode code
point where JCS mandates UTF-16 code-unit order (disagreeing for non-BMP keys);
Python's float repr switches to exponent form at 1e16 where ECMAScript does at
1e21; and Python emits arbitrary-precision integers outside the IEEE-754 double
range.

**Framework.** Canonicalization is unified across three implementations pinned to
one vector file: `modules/core/source/core/text/json-text.cpp`,
`modules/task/runtime/jcs.luau`, `tools/annotate/jcs.py`, against
`tests/vectors/jcs-vectors.txt` — **47 vectors, count independently confirmed**,
the file declaring `count 47` at line 69 so a partial load cannot pass silently.
Expected bytes are V8-derived, not produced by any implementation under test: a
real positive control.

**Which side is wrong.** The consumer, against its own bundle — and the three
variants disagree with each other, which is a defect independent of JCS.

**Premise correction.** The unification is commit `a73bc27` (2026-08-10 23:20),
**not** one of the six commits named as tonight's landings; it is an ancestor of
`c23efd3`, about an hour before `4b955de`.

**Bundle nuance (see B-13).** The bundle states the JCS byte rule **once**, at
`:258-262`, and only for `runtime-artifact.manifest.json`. It is the right rule to
generalise, but the framework's application of it to the registration and session
manifests is a choice, not a quotation.

---

### F-8 — `A-04`'s project half is gated by nothing a consumer runs, and uf-chaos is now really a consumer

**`C-11`'s project half runs everywhere.** `TEST_CASE("contract-control-c11")` at
`contract-suite/source/suite-control-ledger.cpp`:155, a hard-coded member of
`UF_CONTRACT_SUITE_SOURCES` at `cmake/operator-contract-suite.cmake`:34, compiled
into every `uf_add_operator_contract_suite()` call, and genuinely
project-parameterised — `contract-suite/source/harness.cpp`:162 calls
`projectUnderTest(ProjectRole::UnderTest)`. The aggregate CTest
`contract-suite-<PROJECT>` (`:211-216`, `LABELS "CI;CONTRACT-SUITE"`) runs every
case with no `--test-case` filter.

**`A-04`'s does not run anywhere a consumer can reach.**
`TEST_CASE("contract-agent-a04")` at `tests/operator/test-agent-audit-contract.cpp`:1046,
registered at `tests/CMakeLists.txt`:520 inside the framework-internal
`test-contract-operator`. It is **not** project-parameterised: it includes
`project-fixture.hpp` (`:4`) — the framework's own fixture — never
`projectUnderTest()`. `contract-suite/` contains zero occurrences of `a04`. The
whole `tests/` tree sits behind `CPP_BUILD_TESTS` (`CMakeLists.txt`:79-81), which
a consumer building only the suite never sets.

**No longer hypothetical.** uf-chaos builds the suite: `contract/CMakeLists.txt`:12
calls `uf_add_operator_contract_suite(TARGET contract-suite-chaos PROJECT chaos
SOURCES provider.cpp LIBS ${PROJECT_NAME}_image)` with **no `CASES`**, taking the
single aggregate. Its root `CMakeLists.txt`:33 calls itself "the first repository
outside umbraflow to run the Operator contract suite". So uf-chaos runs `C-11`'s
project half and can never run `A-04`'s.

**Two precision corrections.** `contract-agent-a04` *is* gated inside the
framework (`LABELS "CI;CONTRACT"`); the accurate claim is that it is gated by
nothing a **consumer** runs. And "exported" is source-level only: `install(` and
`export(` appear nowhere in the framework's CMake, by design
(`cmake/operator-contract-suite.cmake`:11-16).

**Which side is wrong.** Neither. A framework gap, already correctly identified at
[the consumer attestation](2026-08-11-consumer-attestation.md):687-696 with open
question Q4.

---

### F-9 — The migration report's ownership table still drops the second label on `C-11` and `A-04`

**Arbiter.** `requirements-traceability.md`:101 and `:113` mark both
`` `REQUIRED_CORE` + `PROJECT_CONTRACT` `` — the only two rows in the matrix whose
ownership cell holds two labels.

**Framework.** [The migration report](2026-08-09-runtime-migration-report.md):160
and `:167` give each a single framework owner and purely local CTests. Every other
`PROJECT_CONTRACT` requirement in that table (`:127-135`) carries `EXTERNAL
attest-consumer-dNN`. Neither `C-11` nor `A-04` has any consumer-side verification
ID anywhere in the tree.

**Why it matters.** `requirements-traceability.md`:130-131 makes G0 conditional on
the upstream migration report giving **each** requirement ID an exact owner, schema
path and test ID. The ownership table is the artifact G0 names.

The report's amendment at `:113-115` admits the gap — "It also records two matrix
facts this report does not carry" — so it is knowingly unpropagated. That is
better than ignorance and still a defect.

**Which side is wrong.** The framework. **Correction:** add the consumer half to
both rows.

---

### F-10 — The consumer's obligation count is stated three ways, and none matches the matrix

**The matrix, counted directly.** Exactly **ten** requirement rows carry
`PROJECT_CONTRACT`: `D-01`–`D-08` (lines 53-60), `C-11` (101), `A-04` (113).
(`grep -c` returns 11; one hit is the §1 legend at line 19.)

1. `2026-08-10-next-block.md` originally: "`D-01`-`D-09` are `PROJECT_CONTRACT`"
   → nine. **Wrong on `D-09`.** The correction **landed** in `51970c9`; the
   current text at `:45-61` gives 42 `REQUIRED_CORE` / 8 `PROJECT_CONTRACT`-only /
   1 `PHASED`, matching the arbiter label for label. **Verified correct.**
2. [The consumer attestation](2026-08-11-consumer-attestation.md):668-669 and the
   migration report amendment: "**eleven** requirements, not nine" — counting
   `D-09` as a consumer obligation.
3. The matrix itself: **ten**.

**Where the residual error is.** `D-09`'s ownership cell holds `` `PHASED` ``
(`requirements-traceability.md`:61), and §1 line 20 defines `PHASED` as a
**delivery-staging** semantic — "架构已确定，但按垂直切片逐步开放" — not an
ownership one. The matrix therefore assigns `D-09` **no owner at all**.

The correction fixed the label and kept the obligation, which is the one
combination the matrix does not support. That reading is now being encoded into a
proposed schema: `2026-08-11-consumer-attestation.md`:119 specifies
`attestations[9]` "one per `requirement_id`, `D-01..D-09`", and `:639` says the
set "rejects by requiring nine".

**Which side is wrong.** Partly the framework (over-count), partly the bundle —
see B-6.

**Also.** `2026-08-10-next-block.md`:46-47 still reads "The other **42 are
`REQUIRED_CORE` and every one is ours.**" Two of those 42 are not wholly ours, and
its own correction block at `:49-61` says so — body claim and correction
contradicting on adjacent lines.

---

### F-11 — Policy is never evaluated, and `C-12` is counted done

**Bundle.** `umbraflow-game-automation-final-design.md`:830-840 — high-risk
authorization "至少绑定 … policy artifact hash"; `:700` — "然后运行
capability/policy/approval 并铸造 EffectivePlan".

**Framework.** `2026-08-10-w2-effective-plan.md`:995-997 — "`policyHash` stays a
caller field. It is the one remaining hole"; `:1138-1148` — "nothing parses it…
`required_approvals` is therefore derived in W2 from `risk` alone, through an
Operator-owned table… This is `c12`'s remaining debt, **and `c12` is currently
counted as done**."

Honestly disclosed, which is why it ranks here rather than higher. But a
caller-chosen hash that nothing reads is a field, not a binding, and the
requirement is marked satisfied while the bundle's policy step is absent.

**Which side is wrong.** The framework, on the conformance claim.

---

### F-12 — Requirements quoted with a changed meaning

The brief asked specifically for these, on the grounds that a requirement quoted
wrongly is worse than one not quoted. Seven, all VERIFIED:

1. **"Nothing in the frozen bundle names a ceiling."**
   `2026-08-10-w2-effective-plan.md`:1164-1166 and
   `2026-08-10-w2-w7-reconciliation.md`:1012-1015, both adding "and a search of
   the tree confirms it." `workflow_limits` **is** a bundle `ToolDescriptor`
   member (`umbraflow-game-automation-final-design.md`:631), with `:639` fixing
   its role as the static upper bound, and it also appears on `PlanProposal`
   (`:660`) and `EffectivePlan` (`:667`). The bundle's ceiling is per-tool and
   project-declared; the framework replaced it with a hardcoded global.

2. **"`ui_observation` — both spellings are the authority's."**
   `2026-08-10-w3-snapshot-coordinator.md`:493-494 and `:1018-1021`. A grep of the
   entire bundle returns **zero occurrences of `ui_observation`**. The authority's
   spelling is `ui_snapshot` (main design `:379`, project-layer `:394`).
   `ui_observation` is the framework's own schema member.

3. **"Not resolvable without the v1.9 clause behind `P-03`."**
   `2026-08-10-w6-w7-controller-and-agent.md`:1050-1055. The clause exists and
   says something else: `requirements-traceability.md`:44 requires
   "capability/global/upvalue/import 攻击测试证明不可达", and main design `:887-889`
   requires the privileged surface to **not exist** in the Agent's capability set,
   with `:1265` closing the bypass routes. The bundle's mechanism is
   non-existence proven by attack test; the framework substituted a marking on a
   project-owned catalog and then documented that a project can dissolve the
   requirement by marking everything `Semantic`.

4. **"Two snapshots over an identical world share an `identity_hash`."**
   `2026-08-10-w3-snapshot-coordinator.md`:630-634 and
   `2026-08-10-w2-w7-reconciliation.md`:354-357 and `:733-736`. Main design
   `:469-470` grants that invariance to the **decision basis** only: "相同语义的新
   capture 可以产生新 observation id/revision，但只要这些决策输入不变…".
   `observation_id` is a `SnapshotIdentity` member (`:553`). W3's own landing note
   (`:65-68`) concedes the claim "cannot hold" — but the reconciliation, the
   document that declares itself the winner where the four disagree, still states
   it twice, uncorrected, and once inside DDL comment text that is itself inside
   the schema fingerprint.

5. **"Three of the four triggers."** `2026-08-10-w3-snapshot-coordinator.md`:522-526
   claims acquire/takeover/release is three of four. Main design `:528` lists
   "acquire/takeover/**policy**/**availability** 变化". Release is not among them;
   policy and availability are, and neither is covered. The same document states
   it correctly at `:1031-1035` ("covers the first two"), so it disagrees with
   itself.

6. **"This specification is the first place it is written down"** (the
   offline/online Agent split). `2026-08-10-w6-w7-controller-and-agent.md`:446-448.
   Main design `:331-334` states the split explicitly, and Phase 0 `:1136` makes
   naming the two roles a G0 deliverable. Minor terminology drift alongside: the
   bundle's role is the *offline Annotation Agent*; the framework calls it the
   "offline exploration Agent".

7. **`ExpectedEffect` reused for the Operator effect envelope.** The framework's
   `$defs.ExpectedEffect` is member-for-member the bundle's **`EffectEnvelope`**
   (main design `:643-650`). The bundle reserves `ExpectedEffect` for the opposite
   pole of a pair it forbids merging — project-layer `:500-506`, "`ExpectedEffect`
   来自固定 ContentPack；`ObservedOutcome` 来自新观察；**两者永远分开**", and
   requirement `D-08`. The framework's operator schema now places `ExpectedEffect`
   and `ObservedOutcome` side by side meaning two different layers.

**Which side is wrong.** The framework in all seven. **Correction:** these are
cheap to fix and each one removes a false premise that later work is resting on —
(1) and (3) in particular are load-bearing for F-3.

---

### F-13 — `controlled_target_id` versus `controlled_target_key`: one concept, two spellings, and the code is the drifted side

Raised by the framework's own documentation sweep; verified here as a
cross-repository question.

**The bundle uses one spelling, consistently.** `controlled_target_id` — three
occurrences (`umbraflow-game-automation-final-design.md`:222, :551, :591) —
and **zero** occurrences of `controlled_target_key`. **This is not a bundle
self-contradiction.** The arbiter speaks with one voice.

**The framework's `schema/` agrees with the bundle**: 20 occurrences of
`controlled_target_id`, zero of `controlled_target_key`.

**The framework's C++ disagrees with both**: `controlledTargetKey` 85 times,
`controlled_target_key` 57 times (including at least eight SQLite DDL columns —
`modules/operator/source/operator/ledger.cpp`:616, 644, 649, 661, 684, 748, 897,
956, two of them `PRIMARY KEY`), against `controlledTargetId` 3 times and
`controlled_target_id` 4.

**They are the same value, and the rename is explicit.**
`modules/operator/source/operator/ledger.cpp`:3956 reads
`.controlledTargetId = controlledTargetKey,` and `:1741` then serialises it as
`"controlled_target_id"`. So the internal and DB spelling is `_key`, the wire and
schema spelling is `_id`, and a single assignment bridges them.

**Which side is wrong.** The framework's C++ implementation. The bundle and the
framework's own schemas agree; the code invented a second spelling for one concept
and hides it behind a boundary rename. That is precisely the shim
[`CLAUDE.md`](../../CLAUDE.md)'s "Break it rather than bridge it" forbids —
"no accepting two spellings of one thing" — and the rename at `:3956` is the
reason "the next reader cannot tell which spelling is the real one."

It predates this block, so it is not fallout from tonight. **Correction:** rename
the C++ and the DDL to `controlled_target_id` and delete the bridging assignment.
Roughly 142 sites; no behaviour changes, because the value is already identical.

> **Applied 2026-08-11 in `07abc3e`, exactly as specified.** 14 files, 142 sites,
> zero residual old spellings; the bridge at `:3956` was deleted rather than
> relocated. Two consequences this finding did not have to predict and that a
> later reader needs. The eight DDL columns are inside the canonicalized schema
> text, so the Operator fingerprint moved to
> `sha256:be80aca714a29c976f53d4bdfe39571975a839027cc3efd15822db8a7df3e7b1`
> while `expectedTables` stayed at 23 — a column rename moves the text and not
> the table names — and an `operator-runtime.sqlite` from before the rename now
> fails to open and is deleted, never migrated. `CONTEXT.md` carries
> `controlled_target_id` as the one spelling with both retired forms under
> `_Avoid_`, which is where the next reader should look first.

---

### F-14 — `ARCHITECTURE.md` names two different bundle versions seven lines apart

`docs/ARCHITECTURE.md`:3 — "Amended 2026-08-09: the **v1.7** spec bundle …".
`docs/ARCHITECTURE.md`:17 — "The read-only consumer bundle is **v1.9**; its root
is `c4760bb5…`."

Framework wrong at `:3`. Correction: v1.9.

---

### F-15 — Framework documents still name a deleted function and a superseded fingerprint

**`createOrLoadOperation`**, removed in `93698b4`, has zero occurrences in
`modules/`, `entry/`, `tests/`, `contract-suite/`. It survives in
`docs/plans/2026-08-09-claude-handoff.md`:281,288;
`docs/plans/2026-08-10-w2-effective-plan.md`:136,160,638;
`docs/plans/2026-08-10-w2-w7-reconciliation.md`:726;
`docs/plans/2026-08-10-w3-snapshot-coordinator.md`:119,394,545,760;
`docs/plans/2026-08-10-w6-w7-controller-and-agent.md`:915,941-943,1000;
`docs/reviews/2026-08-10-runtime-hardening-review.md`:18. The review entry and the
handoff are historical records and are fine as such; W3's `:760` ("signature
unchanged") is now false.

**The superseded fingerprint `12f64bff…` is live in nine documents** (excluding
this one): `docs/TODO.md`, `docs/reviews/2026-08-10-runtime-hardening-review.md`,
and `docs/plans/` — `2026-08-09-claude-handoff.md`, `2026-08-10-next-block.md`,
`2026-08-10-w2-effective-plan.md`, `2026-08-10-w2-w7-reconciliation.md`,
`2026-08-10-w3-snapshot-coordinator.md`, `2026-08-10-w4-delivery-join.md`,
`2026-08-10-w6-w7-controller-and-agent.md`. The pre-window value `5738e6f9…`
survives in six. The current value is
`sha256:500c07b10eb263c0f2d6001e0a8b9a90ddd2afd951130cef71f5dbbfbd66085a`
(`modules/operator/source/operator/ledger.cpp`:362).

> **Corrected 2026-08-11 (`07abc3e`).** This paragraph named
> `bda31e4b18…` at `:361` as current. That commit renamed eight DDL columns to
> `controlled_target_id`, which moves the canonicalized DDL text without moving a
> table name, so the count stays at 23 and the constant moved one line. The
> finding this paragraph makes is unaffected and was promptly re-proved by its own
> subject: `bda31e4b18…` reached ten documents as a live value before it was
> superseded, and correcting them was a second sweep. The nine and six counts
> above are re-verified as of this correction and unchanged.

> **Corrected 2026-08-11, again.** This paragraph named `be80aca714…` at
> `:362` as current. This block renamed four DDL columns — two in
> `journal_events`, two in `project_state` — to the journal schema's member
> names, which moves the canonicalized DDL text without moving a table name,
> so the count stays at 23 and the constant still occurs exactly once, at
> `:362`. The nine and six counts above are unaffected by this change;
> `be80aca714…` now joins the superseded set. See
> [journal record binding](2026-08-11-journal-record-binding.md).

**Consumer-side check performed:** uf-chaos pins **no** framework fingerprint
anywhere, so this breadth does not extend across the boundary. VERIFIED —
`12f64bff`, `bda31e4b`, `c691f1d9` and `9377733` all return zero hits there.

---

## 3. Where the bundle contradicts itself

The findings no single-repository review can produce. In each of these, **neither
repository is the drifted side** — the bundle says two things, and somebody had to
choose. Per §1, the bundle's own remedy for all of them is a co-versioned v2.0.

### B-1 — `project_artifact_roots`: three locations, two spellings, framework chose the minority

| Location | Spelling |
|---|---|
| `umbraflow-game-automation-final-design.md`:449 | `project_artifact_roots[]` — bare array, in the `ProjectRegistration { }` unit |
| `umbraflow-game-automation-final-design.md`:1051 | `project_artifact_roots_manifest_hash` — single hash, in §12's `project_registration_hash { }` tuple |
| `uf-chaos-project-layer-design.md`:57 | `project_artifact_roots_manifest_hash { content_pack_hash, recognition_pack_hash }` |

The main design **contradicts itself**; the project-layer design follows the §12
spelling. Two of three sites say `_manifest_hash`; the framework implemented the
array.

**Where the resolution is recorded.** `2026-08-09-runtime-hardening-rewrite.md`:55-77,
executable conformance resolution 2. It is executable, not just prose:
`schema/umbraflow-project-registration-v1.schema.json` requires
`project_artifact_roots`, with `$defs/artifact_root` = `{name, root_hash}`.

**The choice is the better reading and should stand.** §12's own prose at `:1062`
says "项目 artifact roots 分别内容寻址，核心不为任何具体游戏定义固定 root 名称" —
each root separately content-addressed, core defining no fixed root names. An
array of `{name, root_hash}` sorted by UTF-8 bytes satisfies both halves; a single
opaque `_manifest_hash` satisfies neither. It also **subsumes** the project-layer's
nested form: `content_pack` and `recognition_pack` become two named roots without
core knowing either name.

**But the framework's phrasing overstates.** `:76-77` asserts "There is no second
`project_artifact_roots_manifest_hash` shape." The bundle uses that shape twice.
The sentence reads as a statement about the bundle and is false as such.

**Correction.** Keep the array. Restate the resolution as: "the bundle specifies
both shapes, at main design :449 and :1051 and project-layer :57; we implement the
array because §12's prose requires per-root content addressing and core-agnostic
root names" — and raise it for v2.0 per §1.

**Age evidence.** The same contradiction exists in the *committed* v1.4 copy at
HEAD (lines 366 and 1093). It has survived at least 1.4 → 1.9 untouched: a
long-lived editorial defect, not a recent slip, and it will not fix itself in v2.0
unless someone is told.

### B-2 — Post-dispatch `awaiting_approval` has no legal exit, and the audit table is designated sole authority

**Main design §8.4** (`:790-791`): "`AbortBeforeDelivery`、cancel、deadline 或
lease loss 只有在本 Operation 从未出现 `dispatch_started` 时才能进入
cancelled/expired。出现过任何可能送达的 dispatch 后，相同信号只能冻结后续输入并进入
reconciling/ambiguous."

**The failure/recovery audit's table** (`failure-and-recovery-audit.md`:59-77)
gives `awaiting_approval` exactly two out-edges: → `ready` (`:64`) and → `running`
(`:71`). `:67`'s cancel/deadline row is guarded "**从未 dispatch**". `:79-80`
confirms the frozen-plan guard decides only between `ready` and `running`. So a
post-dispatch Operation in `awaiting_approval` that is cancelled, deadlines, or
loses its lease has **no legal transition at all** — which §8.4 forbids.

**And the bundle designates that table as the only one.** Main design `:777-779`:
"Operation 的唯一、穷尽式迁移表见 [失败模式与恢复审核 §3]。**本节不维护第二份状态
图；实现与测试必须从该表生成允许边和 guard。**"

**The framework added the edge and implemented it.**
`2026-08-09-runtime-hardening-rewrite.md`:42-53 records resolution 1; it is
executable at `modules/operator/source/operator/operation.cpp`:68 —
`TransitionRule{AwaitingApproval, PostDispatchAbort, FrozenGuard::Frozen, Reconciling}`.

**Assessment.** The edge is required by §8.4 and the framework's choice is right on
the merits. Procedurally it is exactly what `:777-779` and `:17-18` forbid: the
implementer generated an edge that the designated table does not contain. This is
the single strongest case for issuing v2.0 rather than continuing to resolve
upstream.

*Same shape, lower severity:* `2026-08-10-w4-delivery-join.md` §4.3 adds
`takeoverLease` as a guard for `running → reconciling`. The edge exists
(`failure-and-recovery-audit.md`:69) but its guard list does not name takeover.

### B-3 — `session_manifest_hash` is listed inside the tuple it hashes

`umbraflow-game-automation-final-design.md`:1056 lists it as a member of the §12
tuple; `:1063` states it is "上述 exact tuple 的 canonical root hash". A hash
cannot be a member of its own preimage; as written, §12 is unimplementable.

The framework correctly excludes it (`schema/umbraflow-operator-v1.schema.json`
`SessionManifest`, eight fields). **Not recorded as a resolution** — it should be.

### B-4 — The framework drops two fields §12's "exact tuple" lists, and the bundle argues both ways about them

`:1042` and `:1044` include `host_protocol_version` and
`operator_protocol_version`. The framework's `SessionManifest` omits both
(`modules/operator/source/operator/manifest.hpp`:128-137, and the schema).

The bundle argues against itself: `:1063` calls the tuple "exact" and makes its
canonical root hash the session authority, while `:1061` says "版本号只用于诊断，
不能替代 authority hash". Including them makes diagnostics load-bearing; excluding
them contradicts "exact tuple".

The framework chose exclusion — the right call — but its justification quotes
`:1061`, which says versions cannot *replace* an authority hash, not that they
leave its preimage. **Correction:** record it as a departure in its own right.

### B-5 — `manifest_schema_hash` is in no bundle document at all

VERIFIED by grep across all four bundle documents: **zero occurrences**. The
framework makes it the **first and a required** field of
`ProjectRegistrationManifest` (`2026-08-09-runtime-hardening-rewrite.md`:60;
`schema/umbraflow-project-registration-v1.schema.json` `required[0]`;
`modules/operator/source/operator/manifest.hpp`:26).

Not wrong — a manifest wants a schema tag. But resolution 2 opens with
"`ProjectRegistrationManifest` has **exactly** these fields", presenting an
eleven-field list as bundle-derived when it is ten bundle fields plus one
invention, and the field participates in `project_registration_hash` and therefore
in every consumer's registration identity forever. Neither
`ProjectRegistrationManifest` nor `SessionManifest` is a name the bundle uses at
all.

### B-6 — `PHASED` is a delivery-staging label sitting in an ownership column

`requirements-traceability.md` §4's columns are `ID | 需求 | 归属 | 验收` — the
third is **ownership**. §1 defines three ownership-ish labels and one that is not
an ownership statement: `PHASED` = "架构已确定，但按垂直切片逐步开放" (`:20`).
`D-09` (`:61`) is the only row whose ownership cell holds it, and therefore has
**no stated owner**. Every downstream count of what the consumer owes had to guess,
and both framework guesses (nine, then eleven) differ from the ten rows actually
marked. **Correction:** the bundle needs a separate column, or `D-09` needs a
second label; the framework should record which reading it adopted and why.

### B-7 — What a `ProjectSnapshot` contains: a closed list that two later sections extend

- `:465-467` — "`ProjectSnapshot` … **只包含**" five items.
- `:891` — "`operator.get_snapshot` **同时返回**当前 durable `event_cursor`."
- `:908-910` — "**snapshot 返回当前 available subset 和每个 unavailable reason**."
- `:917-918` and project-layer `:490-491` make availability a **separate** call:
  `snapshot = session.snapshot()` / `tools = session.available_tools(snapshot.token)`.

§7 closes the list; §9.1 and §9.2 each add a member; §9.3 removes one of the two
again. The bundle never says whether §7's `ProjectSnapshot` and §9's "snapshot"
are one object. The framework's `$defs.ProjectSnapshot` has ten required members,
which is defensible under §9 and exceeds §7's "只包含". Because the bundle is
ambiguous about object identity here, this is a bundle defect rather than
framework drift — but it deserves the v2.0 treatment, because if they *are* one
object the plugin receives the snapshot token and Operator-computed availability,
against `:401-404` ("Operator 是最终 availability 的唯一 owner").

### B-8 — How many Agent budgets, and whether "no-progress" is one

- Main design `:496-497`: "action budget、risk budget、time budget、observation
  budget；" followed by "state hash + command fingerprint 的 **no-progress
  detector**" — **four budgets plus a detector**.
- `requirements-traceability.md`:111, `A-02`: "Agent 有
  action/risk/time/observation/**no-progress budgets**" — **five budgets**.

The framework's documents follow `A-02` ("the five budgets"), while its *code*
follows the main design: `AgentBudget` carries five numbers
(`modules/operator/source/operator/agent-profile.hpp`:27-31) and no-progress is a
separate Operator-owned constant `k_agentNoProgressCeiling` at `:64`, deliberately
outside the profile so the agent cannot declare it.

But neither bundle list is what shipped: `maximum_mutations`
(`agent-profile.hpp`:28) matches **no bundle axis**, and it exists because
`ToolDescriptor`'s per-tool bounds do not (F-3). And the bundle's §10.3 minimum at
`:961-968` also names 总工具成本 (total tool cost), for which
`AgentBudgetRemaining` has no counter.

### B-9 — `VERIFIED_PRIMITIVE` is defined and never used

`requirements-traceability.md`:17 defines it; it appears on **zero** of the 51
requirement rows. Legend-only. Harmless alone, but anyone reconciling label
taxonomies across the two repositories will look for a fourth category that does
not exist.

### B-10 — `session_manifest_hash`'s scope is glossed two ways

`:563` — "覆盖 runtime model、ProjectRegistration、项目 artifact roots、tool catalog
和 policy 的固定 hash", omitting `journal_envelope_schema_hash` and
`agent_profile_hash`. `:1063` — the canonical root hash of the §12 tuple, which
includes both. §12 declares itself "exact" and should win; the framework follows
§12. Recorded because a reader who finds `:563` first will build the wrong
manifest.

### B-11 — The Journal example list omits two of the three event types it requires

`:1012-1013` requires baseline/correction/divergence as namespaced JournalEvents;
the example list two lines below (`:1019-1030`) contains `domain.diverged` only.
Project-layer `:510-523` carries all three, and project-layer `:56` fixes
`baseline_event_type: "project.baseline_created"`. The project-layer wins on
completeness.

### B-12 — The idempotency namespace is spelled twice in adjacent lines

`:736` (`CommandRecord`) says `idempotency_namespace`; `:744` says the SQLite
constraint is on `(authenticated_controller_namespace, plugin_id,
project_instance_key, client_request_id)`. `failure-and-recovery-audit.md`:42 and
`requirements-traceability.md`:96 both say "idempotency namespace". INFERRED that
these are one thing.

### B-13 — The JCS byte rule is stated once, for one file, and a different rule for another

The bundle states "RFC 8785 JCS UTF-8, no BOM or trailing newline" **once**, at
`:258-262`, and only for `runtime-artifact.manifest.json`. For
`project_registration_hash` / `session_manifest_hash` it says only "canonical root
hash" (`:457`, `:1063`). For `spec-bundle.manifest.json` it prescribes a
*different* rule at `:1133` — "UTF-8 无 BOM、LF 换行", not JCS — and indeed the
checked-in manifest is pretty-printed with a trailing newline, which is why its
hash is `sha256` of the file as written rather than of a JCS form.

The framework generalises the JCS rule to both manifests. Sound, and harmless in
effect, but presented as frozen when it is an extension.

---

## 4. Authorities that no gate reads

The coordinator asked whether the consumer has a document in the position the
migration report occupies upstream — an authority everything defers to and nothing
checks. It does, and so does the framework, and this is the mechanism behind a
large share of the findings above.

- **Framework:** `2026-08-09-runtime-migration-report.md` is the G0 deliverable and
  no gate reads it, so its rule that it be updated before a landing is enforced by
  nothing. F-9 is the visible symptom.
- **Framework:** the bundle pin itself (F-1b). Eight documents cite the root hash;
  no script verifies it.
- **Consumer:** `2026-08-09-claude-handoff.md` — untracked, at the repository root,
  outside `docs/`, cited by three source modules as the reason they are frozen, and
  read by nothing. F-2 is the visible symptom, and it stayed wrong for a day
  because nothing could notice.
- **Consumer:** the bundle itself, which is untracked and therefore not even
  covered by the consumer's own history.

A document that cannot be wrong is how both repositories got here. The general
remedy is not more review; it is to give each authority one machine-checkable
claim — a hash, a count, a symbol that must exist — so that being wrong has a
visible consequence.

---

## 5. Corrections to the premises this audit was given

Three stated facts did not survive checking. None weakens the findings; all matter
for anyone acting on them.

1. **The schema fingerprint moved five times across the six commits, not four.**
   `4b955de` → `3a406b9d…`, `848e390` → `12f64bff…`, `25f57f9` → `937773366f…`,
   `93698b4` → `c691f1d9bf…`, `c23efd3` → `bda31e4b…`. (`e64c143` did not move it,
   and registered no gate.) "Four" is right only if the window starts *after*
   `4b955de`. Operational consequence, unchanged since `25f57f9`: existing
   `operator-runtime.sqlite` files stop opening; the migration is to delete the
   file. VERIFIED by reading each commit; the value `aa3d356c…` mentioned in
   passing exists in no commit and no document — zero hits tree-wide.
2. **Canonicalization was not unified in any of the six commits.** It is `a73bc27`,
   2026-08-10 23:20, an ancestor with a disjoint diff. The 47 shared vectors are
   real and verified.
3. **`TaskHost::deliver` is `private`.** `modules/task/source/task/task-host.hpp`:266-270,
   `public:` beginning at `:285`, `friend struct TaskHostTestAccess;` at `:193`.
   "Delivery goes through a report only the Host can mint" understates it:
   production has **no route to `deliver` at all** (see F-4).
   `HostDeliveryReport`'s constructor is private with `friend class TaskHost` its
   only friend (`modules/task/source/task/host-delivery.hpp`:85-101), and
   `TaskHostTestAccess` is deliberately *not* a friend of the report — so the
   harness that can call `deliver` cannot fabricate its result. Friendship is not
   transitive, and that is the whole mechanism.

Current consumer-visible signatures, VERIFIED at HEAD, for whoever writes the
consumer-side correction:
`submitCommand(ControllerBinding const&, CommandRequest const&, ValidatedToolInvocation const&) -> Result<AcceptedCommand>`
(`modules/operator/source/operator/ledger.hpp`:547-552);
`pinSession(SessionPin const&, SessionManifest const&, std::optional<AgentProfile> const&) -> Status`
(`:479-484`);
`reserveDispatch(std::string const&, uint64, ControlLease const&, GenerationId, AuthorityDecisionId const&, std::optional<ApprovalGrant> const&) -> Result<DispatchReservation>`
(`:641-649`); `ToolSurface` is a two-enumerator enum class
(`modules/operator/source/operator/tool-invocation.hpp`:44-48); `ControllerBinding`
has a private constructor with `OperatorCoordinator` as sole friend
(`modules/operator/source/operator/controller.hpp`:87-123), its variants being the
`ControllerKind` enum `{Script, Agent, Human}` (`:21-26`).

---

## 6. Coverage: what was compared, sampled, and not reached

Stated precisely, because a drift audit that overstates coverage leaves the gap
unlooked-at forever.

**Compared in full (both sides read end to end):**

- All four bundle documents (2,355 lines total) plus `spec-bundle.manifest.json`,
  against the pin — SHA-256 and byte size, verified twice by two readers.
- `requirements-traceability.md` — all 51 requirement rows transcribed with labels.
- All five framework W-specifications (5,280 lines):
  `2026-08-10-w2-effective-plan.md`, `2026-08-10-w3-snapshot-coordinator.md`,
  `2026-08-10-w4-delivery-join.md`, `2026-08-10-w6-w7-controller-and-agent.md`,
  `2026-08-10-w2-w7-reconciliation.md` — roughly 75 bundle-grounded passages
  extracted and located against the bundle; 19 reported, the remaining ~56
  faithful.
- `2026-08-09-runtime-hardening-rewrite.md` (205 lines).
- `2026-08-10-next-block.md` and `2026-08-09-runtime-migration-report.md` — every
  requirement-ID occurrence checked against the matrix.
- uf-chaos `2026-08-09-claude-handoff.md` (442 lines), root `CMakeLists.txt`,
  `contract/CMakeLists.txt`.
- `schema/umbraflow-project-registration-v1.schema.json` and the `SessionManifest`
  definition in `schema/umbraflow-operator-v1.schema.json`, field by field.
- The six commits `4b955de`, `848e390`, `e64c143`, `25f57f9`, `93698b4`, `c23efd3`,
  read as diffs rather than through any summary.

**Sampled (targeted reads plus exhaustive grep):**

- `2026-08-11-consumer-attestation.md` (786 lines) — §1-§2.1 and §8-§10 read;
  §3-§7 grepped only.
- Framework schema files other than the two above — `$defs` and `required` lists
  extracted by script, not read line by line.
- uf-chaos `page-model.toml` — line 1, root keys, table-name inventory. **5,926
  lines of element bodies unread.**
- uf-chaos's nine `.luau` files — grep-only. The verb inventory is exhaustive by
  construction (all `receiver:method(` pairs extracted, not just `ctx:`), but no
  task logic was read.
- uf-chaos `contract/provider.cpp` (1,146 lines) — includes, type declarations and
  the `projectUnderTest` signature; bodies unread.

**Not reached — the honest gaps:**

- **Implementation bodies were not verified against the W-specs.**
  `ledger.cpp`, `effective-plan.cpp` and `controller.cpp` were read only where a
  specific claim needed checking (the transition rule, the fingerprint, the
  `controlled_target` rename, the budget DDL). Findings F-3, F-4, F-6 and F-11 are
  document-vs-document plus schema/header *shape*; **what the landed code actually
  does beyond its field lists was not confirmed.** F-4 in particular rests on W4's
  own admission rather than on tracing production call paths.
- Whether uf-chaos's 1,146-line `provider.cpp` compiles against framework HEAD. No
  build was run (correctly forbidden); the match is a static read of `ToolSurface`,
  `ProjectVocabulary` and `projectUnderTest` against current headers.

  **Settled 2026-08-11, and the answer is no.** It is pinned to framework
  `4662eed` and owes three repairs, each a member `ProjectUnderTest` gained
  since. Nothing is optional: the two struct members carry no in-class
  initializer that could stand in, and `ProjectFingerprint` has no default
  state at all, so an aggregate initialisation omitting the third does not
  compile.

  1. `vocabulary.uiAction` (`e1f1b78`) — the surface, target and action id a
     contract run may drive. uf-chaos's step intents already name
     `event`/`event.option_1` and `recruit`/`shop.product_1`; the repair is
     copying one of those triples into the vocabulary.
  2. `runtimeArtifact` (`e1f1b78`) — RuntimeModel bytes and the asset closure.
     uf-chaos has both, published and verified: `runtime/artifact/`, whose
     `runtime/check/main.cpp` already drives them through this framework's own
     release reader, publisher and Host.
  3. `probeFrame` — the `ProjectFingerprint` its model declares
     (`base_resolution = [1600, 900]`, `base_dpi = [144, 144]`) and one PNG
     capture at exactly that extent, on which `event` resolves.

  Only the third costs anything new, and the cost is a file rather than work:
  `runtime/check/main.cpp` reads its frames from `assets/screens/`, which is
  gitignored and exists only on the annotation machine, so the bytes a provider
  needs are not in that repository. Two ways out, both theirs to choose —
  commit one 1600x900 screen, or compose one from what is already committed,
  since `runtime/tools/build-artifact.py` records the source rect of every
  asset and each binding's placement is a fixed rect, so pasting the committed
  assets onto a 1600x900 canvas at those rects reproduces a frame the model
  resolves without the corpus. The `event` surface needs exactly one of the
  three speed variants present, which a composite satisfies by pasting one.
- uf-chaos `encounters/source/**`, `content/compiled/**`, `assets/screens/**` —
  bulk data, excluded deliberately.
- Every claim about what CTest *would* run is read from CMake, not from a
  `ctest -N` listing.
- uf-chaos's Python modules outside `modules/content/` and `modules/project_state/`
  were not surveyed for further framework coupling.

**Instability.** `docs/plans/2026-08-10-next-block.md`,
`2026-08-09-runtime-migration-report.md`, `2026-08-10-w2-w7-reconciliation.md`,
`2026-08-10-w4-delivery-join.md` and `2026-08-10-w6-w7-controller-and-agent.md`
were dirty, several with mtimes within minutes of the read and freshly appended
"Landed 2026-08-11" headers; another agent is very likely still appending. Line
numbers are from the final re-read. In uf-chaos, `CMakeLists.txt`, `contract/` and
`page-model.toml` were being written during the audit — `provider.cpp` grew from 4
lines to 1,146 between two reads. Treat consumer line numbers as a 20:40 snapshot.
**The five bundle files did not change** (mtime 2026-08-09 17:28:59), so §0 is
stable.

---

## 7. Which side to trust by default, per class of disagreement

The part of this report that survives these particular findings being fixed.

**1. Requirement classification, ownership and counts → the bundle, always.**
`requirements-traceability.md` is frozen and hash-pinned; it is the only artifact
that cannot drift. Both repositories' restatements have drifted from it in
*opposite* directions (nine, then eleven, against an actual ten). Never trust a
restatement, including a corrected one — F-10 is a correction that fixed the label
and left the count wrong. Count the rows.

**2. Framework API surface, schema shapes and wire formats → the code at HEAD,
never any document in either repository.** Tonight is the proof: the fingerprint
moved five times in six commits and nine documents still carry a superseded value;
`createOrLoadOperation` is named as current in four specs hours after deletion. A
document describing this surface is stale from the moment it is written; date-stamp
it rather than trusting it.

**3. Product behaviour — dispositions, state machines, hash formulas → the bundle,
and when the bundle contradicts itself, neither side: escalate to a bundle
re-version.** This is the correction I would make to the framework's current
practice. Main design `:17-18` forbids the implementer choosing between conflicting
bundle files, and `:777-779` forbids a second state diagram. The four executable
conformance resolutions are each defensible on the merits and collectively
constitute a fork of the specification that only one repository can read. Where a
resolution must ship before v2.0 can be issued, it should at minimum name both
bundle locations, state that the bundle disagrees with itself, and be listed for
the next bundle version — which B-1's wording does not currently do.

**4. What the consumer owes and whether it is actually gated → the framework's
CMake, not either repository's prose.** `cmake/operator-contract-suite.cmake` and
`tests/CMakeLists.txt` are the only artifacts that cannot lie about what runs. F-8
was invisible in prose from both sides and obvious in twelve lines of CMake. A
marking is a claim; a registered test is a gate.

**5. Consumer-side implementation status — what exists, what is frozen → the
filesystem, and check the checkout path first.** F-2 is a document that was
*correct* and is now misleading, and no reading of either repository's prose could
have caught it. Any cross-repository claim about "upstream" should name the
absolute checkout path and the commit it was true of.

**6. Canonicalization → the framework's shared vector file.**
`tests/vectors/jcs-vectors.txt` is V8-derived, count-declared and consumed by three
independent implementations, so it is a positive control rather than a claim. It
outranks prose on both sides — including the bundle's one-line definition at
`:259` — because it is the only statement of the rule that can be run.

**7. Naming, where the bundle uses one spelling → the bundle and the schemas, never
the C++.** F-13's `controlled_target_key` is the whole argument: 142 code sites
against 3 bundle sites and 20 schema sites, bridged by one assignment. When a
concept crosses a serialization boundary, the wire name is the real name, because
it is the one both repositories can see.

**The general rule.** Trust frozen-and-hashed over current-and-narrative; trust
executable over descriptive; trust the wire over the implementation. And when the
bundle speaks twice, trust neither repository — go and read both bundle locations,
because that disagreement is invisible from either side alone and is where the
expensive mistakes live.
