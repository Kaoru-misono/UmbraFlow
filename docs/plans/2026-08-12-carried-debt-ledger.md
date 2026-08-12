# What the archived W-series specifications and reviews still owe

Status: **live work item, and the only live owner of every row below.** Nothing
here is a design and nothing here is a summary of one. Each row is a ruling, a
concession, a refused clause or a measured negative that a document made and
nobody executed, lifted out of that document so it can be archived without the
ruling going with it.

Date: 2026-08-12
Scope: `umbraflow-cpp` only. No consumer-project writes.
Evidence: the documents these rows came from are unchanged in
[`docs/archive/plans/`](../archive/plans/) and
[`docs/archive/reviews/`](../archive/reviews/). Every row names its origin
section, so the argument behind it is one hop away and was not rewritten.

## Why this file exists

`CLAUDE.md`: *"Archiving is blocked while a document still owes something.
Before the move, carry every ruling it made that nobody executed into a live
TODO entry, plan row, or work item, and name that location in the file being
archived."* This is that location.

The rule was written after the 2026-07-25 simplification sweep was archived on
2026-08-01 with its §6 ruling — four `core` facilities to be run through
`evaluate-core-capability` — inside it. The ruling stayed correct, recorded and
inert for seventeen days, and came back as W12 of
[the next block](2026-08-10-next-block.md). The same death happened a second
time one step earlier, to Q5 of the W4 specification: a correct concession, in
the right document, which nobody carried, and requirement `a07` stood falsely
closed on it for twenty hours. §6.1 of the next block is the post-mortem.

**A row leaves this file by being executed or by being ruled, never by being
re-measured.** Several rows below are correct findings whose only defect is that
no one owns them.

## How to read a row

| Column | Meaning |
|---|---|
| ID | greppable; the archived documents cite these |
| Owes | what nobody has executed |
| Where it was ruled | section of the archived document, whose text is unchanged |
| State in the tree | verified 2026-08-12 against `521128b` unless a row says otherwise |

Rows whose **State** says *falsified* are the sharp ones: the document asserts
something the tree contradicts, and until the row is closed the assertion is
still readable somewhere a later agent will trust.

## A. From the W2 EffectivePlan specification

Origin: [`archive/plans/2026-08-10-w2-effective-plan.md`](../archive/plans/2026-08-10-w2-effective-plan.md).
W2's own owed-list already pointed at the next block for its three green
mutations and three unenforced ceilings, and those pointers hold; only what it
did **not** point at is carried here.

| ID | Owes | Where it was ruled | State in the tree |
|---|---|---|---|
| `C-W2-1` | **The plan authority is constructed only in test fixtures, so the exact operator-protocol schema bytes have no production reader.** W2 declined to answer it and said why it is not W2's. Nobody has answered it since. | §11 Q6 | True. The stronger neighbouring fact — no production code holds an `OperatorCoordinator` at all — is in [the next block](2026-08-10-next-block.md) §2, recorded there as `a07`'s residue, and never joined to the schema-bytes half |
| `C-W2-2` | **Three landing deviations exist only in a commit message and in the archived text**: `PlanMintInputs` takes command identity as bytes from the `operations` row rather than a `ValidatedToolInvocation`; `StepMintInputs` takes the stored canonical plan; `ProposedEffect` keeps `opaque_project_payload`. | landing note ¶1 | All three verified at `modules/operator/source/operator/effective-plan.hpp:86,282-292`. Owed: nothing to build — owed is that a reader of the header learns the shape was chosen, not inherited |
| `C-W2-3` | **`T4` is not applicable as written**, because `DecisionBasisParts` carries four content hashes and nothing else — a stronger guarantee than T4 asked for. Recorded nowhere else. | landing note ¶2 | Correct. Carried so the next mutation campaign does not re-run a row that cannot mean anything |

## B. From the W3 Snapshot Coordinator specification

Origin: [`archive/plans/2026-08-10-w3-snapshot-coordinator.md`](../archive/plans/2026-08-10-w3-snapshot-coordinator.md).
W3 had no "what it still owes" section of its own — the claim in
[`plans/README.md`](README.md) that all four specifications carried one was true
of W2 and false of W3. This section is that missing block.

| ID | Owes | Where it was ruled | State in the tree |
|---|---|---|---|
| `C-W3-1` | **`identity_hash` cannot be invariant across two observations of an identical world**, because `observation_id` is one of the frozen `SnapshotParts` members. W3's landing note conceded it; two live artefacts still assert the opposite. | landing note, against §3.5 and §4 | **Falsified and still asserted.** `modules/operator/source/operator/ledger.cpp:706-707` states the invariance *inside the DDL text the schema fingerprint canonicalizes*, so correcting the comment moves the fingerprint and refuses every existing operator database. `archive/plans/2026-08-10-w2-w7-reconciliation.md` §3.5 and §6.2 state it twice more. Also recorded as [cross-repository drift](2026-08-11-cross-repository-drift.md) F-12 item 4 |
| `C-W3-2` | **§3.4's join comment claims a guarantee its own mutation campaign disproved.** `T6a` and `T6b` are each green; only `T6c`, deleting both clauses, is red. The conjunction is guarded; neither clause is. | landing note, against §3.4 | **Falsified and still asserted.** `ledger.cpp:4256-4260` tells the next reader that `obs.project_state_revision = state.revision` "is not redundant with the clause above it"; both clauses are live at `:4279-4280`. Belongs beside its family in [checks that cannot fail](../pitfalls/checks-that-cannot-fail.md) |
| `C-W3-3` | **`T5d` proves nothing in its literal form** — the value it adds is constant across captures in that fixture. It was retargeted to a per-capture value and reported as a substitution. | landing note | Correct; the instance is recorded in no live document, only the general shape |
| `C-W3-4` | **The surface rule `T8` asserts did not exist and had to be written, and its first version passed with the forbidden parameter reintroduced.** A check that could not fail, inside the machinery built to catch checks that cannot fail. | landing note | The rule landed and is correct (`tests/test-runtime-surface.py`, the `createSnapshot` declaration guard). The *outcome* — that its first version was green against the mutation it existed for — reaches no live document |
| `C-W3-5` | **`project_observations` grows without bound.** "Whether observation rows are pruned, and by what rule that does not break the join in §3.4, has no owner." | §9 Q5 | Still true and still unowned. Table at `ledger.cpp:585`, inserted at `:3829`, pruned nowhere |
| `C-W3-6` | **`availability_revision` is blind to policy**, so a policy change does not move the snapshot identity. W3 named this as `c12`'s gap. | §9 Q6 | Open. [The next block](2026-08-10-next-block.md) carries only the *other* half of `c12`'s debt (`ApprovalRequest::policyHash` as a caller field); this half is in no live document. `availability_revision` is still `MAX(sequence)` over `control_transitions` (`ledger.cpp:3945-3953`) |
| `C-W3-7` | **`ui_snapshot` versus `ui_observation` — "Recorded, not resolved."** | §9 Q3 | Open **and** the premise is false: the frozen bundle contains zero occurrences of `ui_observation`; the authority's spelling is `ui_snapshot`, and `ui_observation` is this framework's own invention. See [cross-repository drift](2026-08-11-cross-repository-drift.md) F-12 item 2 |
| `C-W3-8` | **§9 Q4's ruling is half executed.** W6 was to own `available_tools` and `event_cursor` on the snapshot; W6 and W7 landed and neither exists. | §9 Q4, with reconciliation §3.10 | `snapshots` has no `event_cursor` column (`ledger.cpp:708-736`; the INSERT at `:4007-4011` binds 16 columns), and no available-tool set exists anywhere. See also `C-W67-9` |
| `C-W3-9` | **The conditional ruling on `availability_revision` has not been discharged**: the moment an available-tool set exists, `availability_revision` must become *that set's* revision, because it is inside `identity_hash`. A semantic change invisible to both the compiler and the fingerprint. | §8 assumption 7, reconciliation §3.10 | Not yet triggered — no such set exists — so this row is a trap laid for whoever builds one |
| `C-W3-10` | **"Three of the four triggers" is wrong, and the document disagrees with itself.** §3.3 claims acquire/takeover/release is three of four; §9.6 says it covers the first two. The design's four are acquire/takeover/**policy**/**availability**; release is not among them and neither policy nor availability is covered. | §3.3 against §9.6 | Confirmed by [cross-repository drift](2026-08-11-cross-repository-drift.md) F-12 item 5. Joins `C-W3-6` |
| `C-W3-11` | **`createSnapshot`'s single-friend property is untested**, and the document says the §7 falsification is what would notice. It never tested it. | §2.3 | The identical zero for `HostDeliveryReport` **is** carried in [the next block](2026-08-10-next-block.md) §2 and in [checks that cannot fail](../pitfalls/checks-that-cannot-fail.md); this one should be recorded the same way or repaired |

## C. From the W4 delivery-join specification

Origin: [`archive/plans/2026-08-10-w4-delivery-join.md`](../archive/plans/2026-08-10-w4-delivery-join.md).
This is the document whose Q5 died in place. Its open questions are carried in
full, answered or not, because that is the failure this ledger exists to stop
repeating.

| ID | Owes | Where it was ruled | State in the tree |
|---|---|---|---|
| `C-W4-1` | **Should the engine return a delivery-phase result**, so a `beginDelivery` / `authorizeCoordinate` failure becomes `not_delivered` rather than `transport_unknown`? Today the framework under-claims: it reports uncertainty where it has certainty, which widens `Rejected`'s unreachability. Changes a `KEEP` module. | §9 Q1 | Open, unowned. **It bears on `c03`, a requirement this document's `Closes:` line names** — precisely the shape §6.1 of the next block proposes a rule against |
| `C-W4-2` | **Is the DDL fingerprint the sole schema identity?** `PRAGMA user_version` stays `1` across every schema-byte change. | §9 Q2 | Open, unowned in the live set |
| `C-W4-3` | **`releaseLease` may leave a dispatch unanswered.** A controller can release with a NULL outcome outstanding, and nothing resolves it until the next restart. Should `releaseLease` refuse, or resolve? | §9 Q3 | Open. Verified still true: `resolveUnansweredDispatches` is called only from `recoverUncertainDispatches` (`ledger.cpp:2466`) and `takeoverLease` (`:3386`), never from `releaseLease` (`:3417`). The only live mention cites it as evidence *for* a proposed process rule, not as work |
| `C-W4-4` | **Is one `TaskHost` bound to one `controlled_target_id` the intended shape**, or does `m_fence` become a per-target map with every Receipt carrying its target? | §9 Q6 | Open, unowned |
| `C-W4-5` | **Q4's answer has a consumer-visible cost that was never recorded.** The exported suite grew a `TaskHost`, so the Runtime v2 world is now consumer-visible through it. | §9 Q4 and the landing note | True and unrecorded. [The next block](2026-08-10-next-block.md) §5 records the suite's reshaping and not this |
| `C-W4-6` | **`T-10` is unfalsifiable as written** — the mutation was run and reported green. A fifth zero for `c03` / `a07`, beside the four the next block names. | landing note | Correct. The next block lists four W4 zeros; this is the fifth and is in no live document |
| `C-W4-7` | **A second `TaskContext` on one `TaskHost` generation resolves nothing**, because `observe.luau`'s template cache is keyed by the RuntimeModel and outlives the context that registered the handles. Found while implementing, predicted by nobody. | landing note | Reusable failure knowledge with no home. It belongs in [`docs/pitfalls/`](../pitfalls/README.md) |
| `C-W4-8` | **Three assumptions were never re-checked, and one of them was written as a precondition of accepting W4.** §8 A9: "§1.2's claim that five of six operations are serialized by SQLite must be re-checked against W3's `ProjectObservation` composition before W4 is accepted" — W4 was accepted. §8 A5: if several dispatches may be in flight, `resolveUnansweredDispatches` must resolve every NULL row and `T-9`'s `== 1` becomes a count. §8 A11: if snapshot composition needs live UI observation through the Host, W4's §2 boundary answer must be revisited before W3 lands — W3 landed. | §8 A5, A9, A11 | No record that any of the three was performed |
| `C-W4-9` | **The `docs/INDEX.md` half of Q7 was never done.** Q7 said both index entries were owed on acceptance; `plans/README.md` gained one and `INDEX.md` did not. | §9 Q7 | Discharged differently on 2026-08-12: `INDEX.md` no longer lists individual specifications at all, and `plans/README.md` is the canonical listing. Kept as the record of why the row closed rather than was done |

## D. From the W6 controller and W7 Agent specification

Origin: [`archive/plans/2026-08-10-w6-w7-controller-and-agent.md`](../archive/plans/2026-08-10-w6-w7-controller-and-agent.md).
Two of these qualify requirements that document's own `Closes:` line marks
closed.

| ID | Owes | Where it was ruled | State in the tree |
|---|---|---|---|
| `C-W67-1` | **`a01`'s event stream is knowingly incomplete, on a requirement marked closed.** §9.2 assumption 8 said W4's `recordDeliveryOutcome` must append a `ledger_events` row, and that "without that the Agent's subscription silently misses every delivery outcome, and `T-A01-c` would pass while the stream was incomplete." | §9.2 assumption 8 | **Falsified.** `recordDeliveryOutcome` (`ledger.cpp:5766`) appends nothing; the five `appendLedgerEvent` sites are `acquireLease`, `takeoverLease`, `releaseLease`, `submitCommand` and `recordExternalInput`. No Operation state change and no delivery outcome reaches the stream. `ledger.hpp:83-92` justifies three event kinds without stating this consequence |
| `C-W67-2` | **`p03`'s offer side was never implemented, and the live ruling still claims it.** §3.2: "Both halves are required. Offering less is not enforcement." Only the accept side landed. | §3.2, T-P03-b | **Falsified.** `available_tools` appears only in `schema/umbraflow-operator-v1.schema.json:395,421` — zero hits in `modules/`, `tests/` or the suite. `contract-product-p03` asserts the accept side only. [The next block](2026-08-10-next-block.md) §4 still rules that `p03` enforces "never **offers** and never accepts". T-P03-b is not among the seven refusals the landing note lists |
| `C-W67-3` | **`ProgressMarker.elapsed_without_progress_ms` is a required member of a frozen schema definition with no producer and no reader.** Deliberately kept; the ruling to keep it lives only in a landed record. | landing note; reconciliation §7.1(a) | `schema/umbraflow-operator-v1.schema.json:1168,1176` require it; the only reference in the tree is a schema-shape substring assertion. `ProgressMarker` has no C++ producer |
| `C-W67-4` | **Who prunes `ledger_events`? "Unassigned."** Until something does, `oldest_available_cursor` is always `0` and only one branch of `ResyncRequired` is producible. | §10 Q5 | Open. `ledger.cpp:4719-4726` states "Nothing prunes `ledger_events`" at the site; no live document owns it, and the unreachable branch is not recorded as one. Pairs with `C-W3-5` |
| `C-W67-5` | **Can a `SessionMode::Read` session bind an Agent?** "Undecided." | §10 Q2 | Open, unowned |
| `C-W67-6` | **Should no-progress carry a millisecond ceiling?** §5.3 rules it out and says in as many words that the ruling may be wrong. The field it turns on is `C-W67-3`. | §10 Q4 | Open, unowned |
| `C-W67-7` | **`AgentBudgetRemaining` carries one no-progress counter, not the two §5.1 specified**, and **`createSnapshot` gained neither a `ControllerBinding` nor an `observedAt`.** Two of the seven refusals that no live document records. | landing note, refusals 2 and 4 | Both refusals stand; only the `pinSession` refusal reached a live document |
| `C-W67-8` | **`operation_state_changed` is deliberately not a fourth `ledger_events` kind.** The reason is sound and it is the direct cause of `C-W67-1`. | landing note, refusal 6 | Recorded nowhere live |
| `C-W67-9` | **`T-A02-j` was unfalsifiable as written**: a C++ guard sitting in front of a database `CHECK` makes the constraint's mutation green. Repaired by removing the C++ guard. | landing note, refusal 5 | The requirement is fine; the **shape** is a form of "a second mechanism refuses first" and is not in [checks that cannot fail](../pitfalls/checks-that-cannot-fail.md) |
| `C-W67-10` | **"Someone should reconcile the older documents' `FrontEnd` wording; this specification does not edit them."** | §2.4 | Still true. `FrontEnd` has zero hits in `modules/` and `entry/` and survives in [agent front end](2026-08-01-agent-front-end-and-exploration.md), [state layer](2026-08-04-state-layer-and-policy-slots.md) and [three-layer task system](2026-07-29-three-layer-task-system.md) |
| `C-W67-11` | **§10 Q1's premise is refuted by the bundle.** The ruling that a `Semantic` marking is "not resolvable without the v1.9 clause behind `P-03`" is wrong: the clause exists and requires the privileged surface to **not exist** in the Agent's capability set, proven by attack test. The framework substituted a marking on a project-owned catalog. | §10 Q1 | [Cross-repository drift](2026-08-11-cross-repository-drift.md) F-12 item 3 and F-3, both open. [The next block](2026-08-10-next-block.md) §4's ruling was made without this |

## E. From the W2-W7 reconciliation

Origin: [`archive/plans/2026-08-10-w2-w7-reconciliation.md`](../archive/plans/2026-08-10-w2-w7-reconciliation.md).
Its rulings R1-R7 are all discharged or recorded elsewhere. What is carried is
its §8 open list — which [the next block](2026-08-10-next-block.md) §6 discharged
by *pointing back into it* — and four items that live in no document at all.

| ID | Owes | Where it was ruled | State in the tree |
|---|---|---|---|
| `C-R-1` | **`k_workflowCeiling`'s values 64/64/256/64/600000 are invented, "and it is a number an implementer will otherwise silently keep."** | §8 bullet 4, from W2 §11 Q2 | Open, and the bullet's own reasoning is wrong. It says nothing in the frozen bundle names a ceiling; `workflow_limits` **is** a bundle `ToolDescriptor` member, and the bundle's ceiling is per-tool and project-declared where the framework hardcoded a global. See [cross-repository drift](2026-08-11-cross-repository-drift.md) F-3 and F-12 item 1. Live at `modules/operator/source/operator/effective-plan.hpp:64-70` |
| `C-R-2` | **`required_approvals` is collapsed to 0/1 against a schema that types it as an array of approver identifiers.** "Unresolved; larger than W2." | §8 bullet 5, from W2 §11 Q3 | Open. Live at `ledger.cpp:793,808-809` and `effective-plan.cpp:24,315` and in no live document |
| `C-R-3` | **§7.1(c) asked that the `sessions.controller_kind` schema addition be raised as a bundle-root question. There is no record it was ever raised.** | §7.1(c) | `controller_kind` is in the DDL (`ledger.cpp:624-625`) and appears **zero** times in `schema/umbraflow-operator-v1.schema.json`. The bundle has moved v1.9 → v1.12 since |
| `C-R-4` | **A measured negative that exists nowhere else: moving the seam between the two DDL string literals does not move the fingerprint** — only a changed character does. Measured when W6 had to move the seam for MSVC's string-length cap. | §6.3 | Correct, and the only record of what the fingerprint does and does not cover |
| `C-R-5` | **The rule that `k_exactSchemaV1Fingerprint` must never be added to `SCHEMA_AUTHORITIES` in `tests/test-runtime-surface.py`.** | §3.8 and §7.4 | The name landed; the exclusion rule lives only in the archived text. `tests/test-runtime-surface.py` has since **grown** that tuple, so the next editor has no record of why the DDL fingerprint is out |
| `C-R-6` | **The observation-budget decrement must sit inside the same `BEGIN IMMEDIATE` and before the plugin call**, so a refused budget never pays for a derive. Ruled "state it in W6's commit". | §4 item 4 | Recorded in no live document |
| `C-R-7` | **R3's ruling — `snapshots` keeps scalar join columns beside `canonical_parts`, one fact stored twice on purpose — and R5's rule that every DDL change recomputes the fingerprint in the same change and says so in its commit message.** Both are executed and both are load-bearing for the next editor. | §2 R3, R5 | Carried in code at `ledger.cpp:692` and `:352-362`. Named here so the rule survives a rewrite of either comment |

## F. From the third adversarial round

Origin: [`archive/reviews/2026-08-10-third-round-review.md`](../archive/reviews/2026-08-10-third-round-review.md).
Verdict FAIL, 17 findings. Twelve are closed in the tree; **nine of those
closures were recorded in no document at all**, including R3-F1, one of the two
findings the verdict called blocking. Recording them is what let the review be
archived, and they are recorded here rather than in the review because the
review is left exactly as written.

**Closed, verified 2026-08-12, and previously recorded nowhere.**

| ID | What closed it |
|---|---|
| R3-F1 | `contract-state-s05` carries the eight-field perturbation loop the finding asked for, plus a fifth schema assertion for `plugin_environment_hash` (`tests/operator/test-state-contract.cpp:622-670`) |
| R3-F4 | `uf_require_executed_assertions` (`cmake/doctest-gate.cmake`), applied at `tests/CMakeLists.txt` and both conformance runs; the header comment restates the finding |
| R3-F7 | The workspace SQLite schema root is pinned as a checked-in literal on both sides — `tools/annotate/tests/test_backend.py:800` and `modules/operator/source/operator/runtime-installation.hpp:31-33` |
| R3-F8 | One canonicaliser: `_canonical_document` delegates to `jcs_bytes` (`tools/annotate/store.py:109-124`) |
| R3-F10 | `tests/task/test-confined-file.cpp:293-297` asserts the `refuse` kind for `""`, `"."`, `".."` and both separators |
| R3-F12 | The case is renamed to say what it asserts (`tools/annotate/tests/test_backend.py:1574`) |
| R3-F13 | `scripts/fix_format.py` gained raw-string protection, naming the `R"sql(...)"` fingerprint as the reason |
| R3-F15 | `.clang-tidy` excludes `third_party` and `.?worktrees`, and documents the version gate on `ExcludeHeaderFilterRegex` |
| R3-F17 | The overstating comment was replaced with the correct property (`tests/task/test-confined-file.cpp:262-269`) |

**Still owed.**

| ID | Owes | State in the tree |
|---|---|---|
| `C-R3-1` | **R3-F5 is accepted permanently and its record is a CMake comment.** The `contract-` versus `schema-` classification is not mechanically checkable and is caught by review and by nothing else. | Stated at `tests/CMakeLists.txt:58-66` and in no document. A `contract-` name is an unverified promise, which is the whole subject of [checks that cannot fail](../pitfalls/checks-that-cannot-fail.md) |
| `C-R3-2` | **R3-F6 leaves a live instruction for whoever recombines this history: `dcc43b5` does not compile.** It calls `ConfinedRoot::removeTree` and `childNames`, which arrive in `cec8898`. Fold the extension into `dcc43b5`, or order `cec8898` first. | Carried in [the next block](2026-08-10-next-block.md) §7; repeated here because §7 is a corrections section a reader of open work will not open |
| `C-R3-3` | **R3-F9's residue is open and carried nowhere.** `modules/cli/source/cli/explore-protocol.cpp:410` passes a raw Luau string to `appendJsonString` with no `isValidUtf8`, so invalid UTF-8 passes through on the explore emit path; `:405`'s `{:.17g}` is retained with a rationale rather than made shortest-round-trip. | The closed half — three JCS spellings held to one vector set — is in [checks that cannot fail](../pitfalls/checks-that-cannot-fail.md) |
| `C-R3-4` | **R3-F14's two limits stand**: the `cppcoreguidelines-pro-type-member-init` suppression proof was never committed as a `static_assert`, so a default constructor added to `PixelRect`, `PixelPoint`, `FrameId` or `ContentHash` later turns the suppressions into silent holes; and `NOLINTNEXTLINE` sits above the record rather than the member, so a bare new field is swallowed. | Carried in [the next block](2026-08-10-next-block.md) §7 and in [checks that cannot fail](../pitfalls/checks-that-cannot-fail.md) |

## G. From the consumer-onboarding measurement

Origin: [`archive/plans/2026-08-11-consumer-onboarding.md`](../archive/plans/2026-08-11-consumer-onboarding.md).
Superseded in shape by [a project is a directory of data](2026-08-11-project-as-data.md)
and kept for its measurement of what the deleted C++ surface cost a consumer.
Its status line says four of its ten questions are moot and one stands as
project-as-data's Q3. Twelve of its rulings were executed and five more were made
moot by the correction; what follows is what was neither.

| ID | Owes | Where it was ruled | State in the tree |
|---|---|---|---|
| `C-CO-1` | **The background-only rule is a product invariant that no compiled contract case can detect, and it must become part of what a consumer attests** — a tenth requirement, or a field on the existing nine. Ten Win32 APIs are forbidden by a Python source scan of *this* repository; a consumer's own code is scanned by nothing. | §8.2, its Q9 | **Dropped, not mooted.** [Consumer attestation](2026-08-11-consumer-attestation.md) still specifies exactly nine requirements and names none of this; `scripts/check_safety.py:47-50` still holds the rule with no consumer-facing route. This is the one genuinely unexecuted ruling in that document |
| `C-CO-2` | **The `attestations`-root trap.** A registration naming an artifact root without the blob beneath it fails at `loadPlugin` with an artifact-closure message that does not say what is missing. The document asked for one sentence in `provider.hpp`. | §9 J2 | The recommendation's target file was deleted with the provider surface, so the sentence has nowhere to go and the trap is untreated. The `attestations` root itself is still unruled — [consumer attestation](2026-08-11-consumer-attestation.md) §10 Q1 |
| `C-CO-3` | **Its own §12 owed two index entries and neither was made**, so a 1,213-line document sat in `docs/plans/` listed in nothing. [Project as data](2026-08-11-project-as-data.md) even places a cross-reference "beside the consumer-onboarding entry", which never existed. | §12 | Closed 2026-08-12 by the archive move: it is listed in [`plans/README.md`](README.md) under archived plans, with the date. Recorded here because the *cause* — a document can enter `docs/plans/` and be indexed by nothing — is the same defect as [cross-repository drift](2026-08-11-cross-repository-drift.md) §4 and is not fixed by one entry |
| `C-CO-4` | **Its status line and §6.4 are stale in the archive.** Both say Q3 "stands as project-as-data's §7 Q1" and that a complete RFC 8785 canonicaliser is "still open" with "both fixtures still faking it with an allowlist". | status line, §6.4 | Both were ruled and landed: `evaluate-core-capability` refused `core`, the canonicaliser landed as `modules/json` (`a0ae304`, `value.hpp:114,121`), and `974396e` deleted both faking exemplars. Stated in the archived file's closure note rather than by editing its body |

## What this ledger does not carry

Two things, deliberately.

**Rows already owned elsewhere are not duplicated.** The four W4 zeros, W2's
`T2`/`T10`/`T13`, `requireLiveBinding`'s masking conjuncts, the budget-presence
invariant, the three stored-and-unenforced ceilings, `StepKind::Wait`,
`ApprovalRequest::policyHash` and the harness defects are all carried in
[the next block](2026-08-10-next-block.md) §2 or in
[checks that cannot fail](../pitfalls/checks-that-cannot-fail.md), which are
their right homes. Copying them here would make two records of one debt, which is
the failure mode one level up from the one this file fixes.

**Nothing here is a rewrite of the archived text.** Where a row says a document
asserts something false, the assertion is left standing in the archived file. The
pointer runs one way, from here to there.
