# W4: join Host delivery to the ledger

Status: **landed 2026-08-11** — additive half `e64c143`, ledger join `25f57f9`.
Everything below is the pre-landing specification and is left as written.
Date: 2026-08-10 (landed 2026-08-11)
Closes: `c03 a07`. **`a07` was reopened and closed again the same day — see the
note directly below.**

> **Reopened 2026-08-11 (`07abc3e`): this document closed `c03` and half of
> `a07`, and Q5 said so before the row was marked closed.** `a07`'s acceptance
> text has two clauses. `contract-agent-a07` proves the second — an in-flight
> dispatch is explicitly reported — and §4.3 and §4.4 below specify exactly that.
> The first clause is structural: a human takeover and Host delivery share one
> target serialization. Nothing joins them. `takeoverLease`'s transaction and
> `TaskHost::adoptControlFence` have **no call edge between them anywhere, in
> production or in test**, and `operator::controlFence(ControlLease const&)` —
> the one API that converts a lease into a Host fence — has zero production
> callers. §1.2 below is where the divergence entered: it relocates the
> linearization to SQLite commit order and rules operation 2 out of the database
> deliberately and correctly, which leaves the Host side to be joined by
> something else. Nothing was.
>
> The vacuity defence in the landing note below stands as far as it goes and no
> further: production cannot dispatch, so nothing violates the invariant today,
> but that is an argument about a join that was never assembled and a future
> wiring inherits the window intact.
>
> **Q5 conceded this and asked that it reach the requirement matrix. It never
> did**, and the row carried no caveat until `07abc3e`. The reopened row, the two
> ways it closes, and the rule proposed so the next concession does not die the
> same way are in [the next block](2026-08-10-next-block.md) §2 and §6.1. Nothing
> below is rewritten; Q5 was right when written and is still the finding.
>
> **Closed again 2026-08-11 (`bed456f`), the same day, on a corrected reading.**
> The reopening above substituted `a07`'s 需求 sentence — "human takeover and
> Host delivery share one target serialization" — for the first of its two
> 验收 clauses. Both clauses live in the 验收: "takeover 返回后旧 fence 不可开始
> 新 dispatch；在途 dispatch 被明确报告." The first clause is `reserveDispatch`'s
> live-lease predicate, not a call edge into `TaskHost`. `requireLiveLease`
> already ran inside the same `BEGIN IMMEDIATE` serialization `takeoverLease`
> commits in, before this reopening; what was missing was a test that ran the
> schedule — no case took over and then attempted a reservation on the
> displaced lease. `contract-agent-a07` was extended to do exactly that, and the
> case falsifies both halves by mutation. **`a07` is closed; 42 of 42
> `REQUIRED_CORE` requirements are.**
>
> Q5's own finding stands and is separate from this: no production code adopts
> a control fence at all, so the Host side of the takeover/delivery join is
> proved by tests and reachable by nothing else. That is not a gap in `a07` —
> both its acceptance clauses close at the ledger layer, inside
> `reserveDispatch` and `recordDeliveryOutcome`, without needing a Host to
> adopt anything — it is
> [F-4 in the cross-repository drift review](2026-08-11-cross-repository-drift.md),
> a deliberate divergence from the bundle's one-function-one-mutex mechanism
> (W4 §1.2 relocated the linearization to SQLite commit order) rather than an
> unmet acceptance clause. See [the next block](2026-08-10-next-block.md) §2,
> "The residue, stated once so nobody re-derives it."
Depends on: W2 (hard, see §8), W3 (sequencing only, see §8)
Scope: `umbraflow-cpp` only. No consumer-project writes.

W4 makes one sentence true: *the ledger learns that an effect happened, or
provably did not, only from the call that could have caused it.* Today the two
halves of that sentence live in different modules and are joined by nothing.

`docs/plans/2026-08-09-runtime-hardening-rewrite.md` is the authority above this
document. `docs/plans/2026-08-10-next-block.md` §2 assigns `c03` and `a07` here
and §4 rules that `OperatorCoordinator` grows rather than gaining a sibling.

> **Amended 2026-08-10: read §1.2 of
> [the W2-W7 reconciliation](2026-08-10-w2-w7-reconciliation.md) first.**
> `contract-control-c03` and `contract-agent-a07` do not exist; `dcc43b5`
> renamed them `schema-control-c03` and `schema-agent-a07`. Every row of §7's
> mutation table names a gate W4 must **create**, migration report first, rather
> than one it can extend.

> **Amended 2026-08-11: both dependencies have landed, and one of W4's own
> tasks was done for it.** W3 is `4b955de` and W2 is `848e390`, so nothing
> sequences this item any longer. `55bd564` extracted the Runtime v2 fixture
> into `tests/support/runtime-v2-fixture.hpp`, which §7 and the work-item row
> counted as W4's cost; take it as given rather than re-deriving it. Two
> signatures this document reads changed underneath it — `createSnapshot` is
> now `(lease, plugin, observation)` and `reserveDispatch` takes no caller
> hashes — so re-read §5's call sites in the tree before editing. The order
> "migration report first, then `tests/CMakeLists.txt`" is unchanged and was
> inverted by both landings; do not inherit that.

> **Landed 2026-08-11 in `e64c143` and `25f57f9`. Read what follows as the plan,
> not as the tree.** `c03` and `a07` both own a `contract-` gate; `c03` is closed
> and `a07` is not — see the reopening note at the top of this document. Every
> "Corrected 2026-08-11 against the landed tree" note
> below was written against `848e390` and predates this landing, so where one of
> them states a current value — the fingerprint above all — read it as of that
> commit. After W6 and W7 the tree carried
> `sha256:bda31e4b18a8096b28e5208f5988dea8658bea9d7917d78cd8655d4f581a8559` over
> 23 tables; W4's own recomputation was `937773366f…` over the
> same 20 tables, because the `dispatches` DDL text changed without a table being
> added, exactly as §6 predicted. *(Corrected 2026-08-11: `07abc3e` moved it
> again, to
> `sha256:be80aca714a29c976f53d4bdfe39571975a839027cc3efd15822db8a7df3e7b1` over
> the same 23 tables, renaming eight DDL columns to `controlled_target_id`. Read
> every `controlled_target_key` below as that spelling: the C++ one is gone and
> the bridge that carried both was deleted rather than relocated. This block
> moved it once more, to
> `sha256:500c07b10eb263c0f2d6001e0a8b9a90ddd2afd951130cef71f5dbbfbd66085a`,
> over the same 23 tables, renaming four DDL columns in `journal_events` and
> `project_state` to the journal schema's member names. See
> [journal record binding](2026-08-11-journal-record-binding.md).)*
>
> **What this document specified and the implementation refused.**
>
> - **Q4's answer is "grow a Host", not "drive `reconciling` through a
>   takeover".** §7's note and the fixture row for `reconcilingOperation` both
>   route the exported suite around its missing Host that way. It cannot be
>   done safely: every Host-less route to a resolved dispatch moves the fence,
>   which would have made an existing approval case pass for the wrong reason —
>   a check that cannot fail, introduced on purpose to avoid a fixture. The
>   exported suite grew a Host instead, so the Runtime v2 world is now
>   consumer-visible through it, which is the cost Q4 named and the one worth
>   paying.
> - **T-10 is unfalsifiable as written.** It asks to drop `AND delivery_outcome
>   IS NULL` from the resolution `UPDATE`, but the scan that reaches that
>   `UPDATE` has two filters that each independently exclude the case, so only
>   removing both goes red. The mutation was run and reported green rather than
>   quietly re-aimed.
> - `operator::DeliveryOutcome` is **deleted** rather than kept beside
>   `task::DeliveryOutcome`. One spelling, no alias.
> - The two-generations ruling was followed and one clause of the
>   reconciliation's reasoning for it was not; see
>   [§7.2 there](2026-08-10-w2-w7-reconciliation.md).
>
> **Production is strictly narrower than this document leaves it, not merely
> unchanged.** `mintClickReceipt` now refuses while no control fence has been
> adopted and nothing in production adopts one, where previously the fence
> started at 1 and minting was permitted. Q5 therefore reads more strongly than
> it was written: the Host half of `a07` is proved by tests and reachable by
> nothing else.
>
> **Both halves' mutations were run — 18 in `e64c143`, 19 in `25f57f9` — and
> the green ones are the report.** The four zeros
> this item carries are recorded beside the requirements they qualify in
> [the next block](2026-08-10-next-block.md) §2, and their repeatable shapes in
> [checks that cannot fail](../pitfalls/checks-that-cannot-fail.md): the lease
> identity spelled three times, the takeover-scoping property that no fixture
> can express, `dispatches.delivery_reason` written and read back by nothing,
> and `HostDeliveryReport`'s friend count, which no test can express at all and
> which was confirmed empirically — a second friend compiles and every case
> stays green.
>
> One finding fell out that this document did not predict and that is
> independent of it: a second `TaskContext` on one `TaskHost` generation
> resolves nothing, because `observe.luau`'s template cache is keyed by the
> RuntimeModel and outlives the context that registered the handles. It forced
> the positive control onto its own Host.

## 1. The linearization argument

### 1.1 What must be totally ordered

Six operations touch the control state of one `controlled_target_key`. Any two
of them that can disagree about whether an effect happened must be totally
ordered:

| # | Operation | What it does to the control state |
|---|---|---|
| 1 | `OperatorCoordinator::reserveDispatch` | creates the right to deliver once |
| 2 | `TaskHost::deliver` | consumes that right and produces the only admissible statement about the effect |
| 3 | `OperatorCoordinator::recordDeliveryOutcome` | turns that statement into a ledger fact |
| 4 | `OperatorCoordinator::takeoverLease` | withdraws the right |
| 5 | `OperatorCoordinator::commitReconciliation` | reads the accumulated facts and writes the Journal |
| 6 | `OperatorCoordinator::recoverUncertainDispatches` | the restart's stand-in for 3 |

### 1.2 What serializes them

Operations 1, 3, 4, 5 and 6 are `BEGIN IMMEDIATE` transactions on one SQLite
file (`Transaction::begin` in `modules/operator/source/operator/ledger.cpp`).
SQLite admits one writer at a time, so those five are totally ordered by commit
order, and that order is the linearization.

Operation 2 is not a database operation and must not become one — making
`OperatorCoordinator` able to call `TaskHost::deliver` would open a production
mutation path, which §5 of `2026-08-10-next-block.md` closes by construction.
Instead operation 2 is *bracketed*:

- it cannot start without a `DispatchAuthority` that only a committed 1 mints;
- its product is admissible to 3 only while the fence that authority names is
  still the fence the lease row holds.

So the bracket `[1, 3]` is atomic-or-void: if 4 commits inside it, 3 is refused
and the dispatch is already resolved. `TaskHost` is single-threaded by contract
(`CycleLedger`: "NOT thread-safe: every method runs on the VM's owning thread"),
so operation 2 is ordered against itself without further machinery.

### 1.3 How they interleave today

Delivery. `reserveDispatch` commits a `dispatches` row with
`delivery_outcome NULL` and moves the Operation to `running`. `TaskHost::deliver`
then runs with no ledger identity at all: its `DeliveryAuthority` is
`{hostNonce, fence}`, both minted by the Host itself
(`modules/task/source/task/task-host.hpp:126-130`). `recordDeliveryOutcome`
finally takes a caller-chosen `DeliveryOutcome` enum and a lookup that reads,
in full (`ledger.cpp:3256-3258`):

```sql
SELECT o.state, o.revision, o.frozen_plan_hash, d.delivery_outcome
FROM operations o JOIN dispatches d ON d.operation_id=o.operation_id
WHERE o.operation_id=?1 AND d.dispatch_sequence=?2
```

No session, no epoch, no lease, no fence. Any holder of an operation id and a
sequence number can write `delivered` or `not_delivered`, having delivered
nothing. That is `c03`: the property holds inside `task` — one Receipt, one
`deliver`, one erase — and stops at the module edge.

Takeover. `takeoverLease` (`ledger.cpp:2270-2406`) bumps
`fencing_high_water`, replaces the `control_leases` row, and appends a
`control_transitions` row with `transition='takeover'`. It touches neither
`TaskHost::m_fence` nor any in-flight `dispatches` row. The displaced
controller therefore keeps live pending Receipts, keeps a deliverable Host, and
keeps the ability to write a delivery outcome. That is `a07`.

Reconcile. `commitReconciliation` is the one path that already fences: it joins
through `k_liveControllerJoin` and requires `session.active=1` and the current
epoch (`ledger.cpp:3562-3565`). The displaced controller cannot write the
Journal. It can, today, write the delivery fact the Journal will later be
reconciled against.

### 1.4 How they interleave after W4

- 1 mints a `task::DispatchAuthority` carrying the ledger identities.
- 2 consumes exactly one Receipt and returns a `task::HostDeliveryReport`
  constructible only by `TaskHost`.
- 3 takes `(ControlLease, expectedRevision, HostDeliveryReport)` and runs the
  same `k_liveControllerJoin` `commitReconciliation` already runs, plus the
  lease identity, the fencing token, and the `authority_decisions` row the
  reservation wrote.
- 4 additionally resolves, in the same transaction that bumps the fence, every
  unanswered dispatch on that target to `transport_unknown` and moves its
  Operation to `reconciling`.

A report produced before a takeover and presented after it is refused twice
over: the lease predicate no longer matches, and the `delivery_outcome IS NULL`
CAS no longer holds. Neither refusal depends on the other, which is what makes
each independently falsifiable (§7).

## 2. The module-boundary answer

`scripts/check_modules.py` builds a graph from the `[dependencies*]` sections of
`modules/*/manifest.txt`, rejects self-dependency (line 174) and rejects any
cycle (`find_cycle`, lines 95-122, reported at line 180). The relevant edges,
read from the manifests:

| Module | `public` | `private` |
|---|---|---|
| `core` | — (the checker rejects any dependency, lines 158-161) | — |
| `domain` | `core` | — |
| `trace` | `core domain` | — |
| `script` | `core domain` | `Luau.VM Luau.Compiler` |
| `engine` | `core domain ocr trace vision` | — |
| `task` | `core domain engine script trace` | `image Luau.VM Luau.Compiler Luau.Ast` |
| `operator` | `core domain task` | `trace script operator_sqlite3` |

`operator -> task` already exists and is public; `ledger.hpp` already includes
`<task/runtime-model-file.hpp>`. The reverse edge `task -> operator` would produce
the cycle `task -> operator -> task` and fail `check_modules.py`.

**Therefore `operator` owns the join.** Concretely:

- every type that crosses the boundary is declared in `task` and named by
  `operator`;
- `task` never names an `operator` type, not even in a friend declaration —
  a `friend class uf::operator_runtime::OperatorCoordinator;` inside `task`
  would need a forward declaration of an upstream namespace, which is the
  boundary violation the graph exists to prevent even though the checker reads
  only manifests;
- the caller that sequences 1 → 2 → 3 is a test, and stays a test (§4.4).

## 3. The fence

### 3.1 The existing fragment, quoted exactly

`modules/operator/source/operator/ledger.cpp:885-896`:

```cpp
// An Operation may only be advanced by the session that owns it, while
// that session is still active at this process epoch AND still holds
// the lease on its target. The lease clause is separate from the epoch
// one because takeoverLease replaces the lease row without deactivating
// the session it replaced: a human takeover would otherwise leave the
// displaced controller able to append to the Journal.
constexpr auto k_liveControllerJoin = std::string_view{
    "JOIN sessions session ON session.session_id=o.session_id "
    "JOIN control_leases lease "
    "ON lease.controlled_target_key=o.controlled_target_key "
    "AND lease.session_id=o.session_id "
};
```

It is a JOIN fragment only; the callers supply the predicate. Both existing
users pair it with `AND session.active=1 AND session.session_epoch=?` bound to
`m_impl->sessionEpoch` (`transitionOperation`, line 2896; `commitReconciliation`,
line 3562). Note that the fragment as written joins the lease row but compares
none of its columns, so on its own it proves "some lease of this session is on
this target", not "this lease".

### 3.2 What "inside the fence" means for a delivery outcome

Exactly one transaction, the one `recordDeliveryOutcome` already opens, must
satisfy all of:

1. the Operation's session is `active` at `m_impl->sessionEpoch`;
2. the live `control_leases` row for the Operation's target is the lease the
   caller presented — same `lease_id`, `fencing_token`, `revision`,
   `session_epoch`;
3. the report was authorized by that same lease — the report's
   `controlledTargetKey`, `leaseId`, `sessionEpoch` and `fencingToken` equal the
   presented lease's, checked in C++ before the statement so the failure names
   the reason;
4. the `authority_decisions` row the reservation wrote still exists with the
   report's `authority_decision_id`, that dispatch sequence, that `lease_id`,
   that `fencing_token` and that `session_epoch`;
5. the `dispatches` row still has `delivery_outcome IS NULL` and the report's
   `frozen_plan_hash`;
6. the Operation revision equals `expectedRevision`.

The new lookup (`k_liveControllerJoin` reused verbatim, the rest appended):

```cpp
"SELECT o.state, o.revision, d.delivery_outcome FROM operations o "
+ std::string{k_liveControllerJoin}
+ "JOIN dispatches d ON d.operation_id=o.operation_id "
  "JOIN authority_decisions a "
  "ON a.authority_decision_id=d.authority_decision_id "
  "WHERE o.operation_id=?1 AND d.dispatch_sequence=?2 "
  "AND session.active=1 AND session.session_epoch=?3 "
  "AND o.controlled_target_key=?4 "
  "AND lease.lease_id=?5 AND lease.fencing_token=?6 "
  "AND lease.revision=?7 AND lease.session_epoch=?3 "
  "AND a.dispatch_sequence=?2 AND a.authority_decision_id=?8 "
  "AND a.lease_id=?5 AND a.fencing_token=?6 AND a.session_epoch=?3 "
  "AND d.frozen_plan_hash=?9"
```

Binds: `?1 ?2 ?8 ?9` from `report.authority()`; `?4 ?5 ?6 ?7` from the presented
`lease`; `?3` from `m_impl->sessionEpoch`. The existing outcome CAS
(`ledger.cpp:3287-3288`, `... AND delivery_outcome IS NULL`) is unchanged and
remains the second, independent refusal.

### 3.3 The outcome vocabulary, and why only one value proves absence

`modules/operator/source/operator/ledger.cpp:3653-3688`, in
`commitReconciliation`:

```cpp
// "Journal/outcome 证明部分 effect" -- the outcome half matters as
// much as the Journal half, and I-13 wants every possible external
// effect proven ABSENT. Only not_delivered is that proof: a NULL
// outcome is a dispatch nobody has answered for, transport_unknown
// is the recovery path's way of saying it does not know, and
// delivered says it happened. Any of the three leaves Rejected
// claiming more than the ledger can support.
UF_TRY_VALUE(
    effectQuery,
    prepare(
        m_impl->database.get(),
        "SELECT 1 FROM journal_events WHERE operation_id=?1 "
        "UNION ALL "
        "SELECT 1 FROM dispatches WHERE operation_id=?1 AND ("
        "delivery_outcome IS NULL OR delivery_outcome<>'not_delivered') "
        "LIMIT 1"
    )
);
```

W4 does not weaken this. It supplies the missing half: today the only writer of
`not_delivered` is a caller passing an enum, so the strongest claim in the
ledger is also the cheapest to make. After W4 `not_delivered` can be written
only from a `HostDeliveryReport` whose outcome is `NotDelivered`, and
`TaskHost::deliver` produces that value in exactly one situation — it consumed
the Receipt and never called into `TaskContext` at all (§4.2).

## 4. The two crossing values, and the takeover path

### 4.1 `DispatchAuthority` is plain data; `HostDeliveryReport` is not

The asymmetry is deliberate and is the whole safety argument:

- Presenting a **forged authority** to the Host can only make the Host refuse.
  It cannot make the Host act, because acting still requires a Receipt the Host
  itself minted, and the ledger still checks every field of the authority
  against its own rows. So `DispatchAuthority` is an aggregate anyone can build.
- Presenting a **forged report** to the ledger would grant a fact. So
  `HostDeliveryReport` has a private constructor whose only friend is
  `class TaskHost`. `TaskHostTestAccess` is deliberately *not* a friend of the
  report: it can reach `TaskHost`'s privates, so it can call `deliver`, but it
  cannot fabricate what `deliver` returns. Without that exclusion the tests in
  §7 would be unfalsifiable.

The same reasoning covers `ControlFence`: raising a Host's fence only ever
withdraws authority (it invalidates pending Receipts), so a forged high fence
costs its forger and nobody else, and a forged low one is refused by
monotonicity.

### 4.2 What `deliver` reports

`TaskHost::deliver` classifies by *how far it got*, never by inspecting an error
kind:

| Result | Condition |
|---|---|
| `Err` | the authority or Receipt is not this Host's, or names another target, generation, epoch or fence, or the Receipt ordinal is unknown. Nothing was consumed and there is nothing to record. |
| `Ok(NotDelivered, reason)` | the Receipt was consumed — the linearization point passed — and `TaskContext::deliverReceiptClick` was never called: stale binding, expired freshness, mismatched cycle. Proof of absence. |
| `Ok(Delivered)` | `deliverReceiptClick` returned `Ok`; the report carries the `engine::ActReceipt`. |
| `Ok(TransportUnknown, reason)` | `deliverReceiptClick` returned `Err`. |

The last row deliberately under-claims. `EngineSession::clickPoint`
(`modules/engine/source/engine/session.cpp:1195-1245`) fails before the sink
(`beginDelivery`, `authorizeCoordinate`), at the sink (`m_actionSink->click`),
and *after* the click has landed (`UF_TRY(emit(clickEvent))` at line 1233, which
runs after `observation.m_invalidated = true`). Its `Result` cannot separate
those three, and inferring absence from an error kind is exactly the guess the
ledger must not make. Every `Err` from the click path therefore becomes
`transport_unknown`. See open question Q1.

### 4.3 `a07`: how a takeover enters the same linearization

Ledger side, inside `takeoverLease`'s existing transaction, after the
`control_leases` and `control_transitions` writes and before `commit()`:

resolve every unanswered dispatch on this `controlled_target_key`, using the
same per-row checked-increment CAS pattern `recoverUncertainDispatches` already
uses (`ledger.cpp:1539-1625`). Both callers are folded into one helper in the
anonymous namespace so there is one spelling of "resolve an unanswered
dispatch":

```cpp
// Drives every dispatch nobody has answered for to transport_unknown and its
// Operation to reconciling. `controlledTargetKey` empty means every target,
// which is what a restart sweeps; a takeover names the one target it seized.
// Never not_delivered: a dispatch the Host may already have posted is exactly
// what the third value exists for.
[[nodiscard]]
auto resolveUnansweredDispatches(
    sqlite3* database,
    std::string_view controlledTargetKey,
    std::string_view reason
) -> Result<uint64>;
```

Host side, in the same process: `TaskHost::adoptControlFence` raises the Host's
fence to the takeover's `fencing_token`, which invalidates every pending
Receipt (they carry the fence they were minted under) and blocks minting a new
one until an authority with the new token arrives.

### 4.4 The in-flight case, concretely

The requirement is the race, so state it as a schedule. `D` is one dispatch of
Operation `O` on target `T`, reserved at fence `F` by lease `L`.

```
t0  reserveDispatch          commit: dispatches(O,1) outcome=NULL, O=running rev=R1,
                                     authority_decisions(A) lease=L fence=F
t1  TaskHost::deliver        no ledger write; Receipt consumed; report P
                             (P.authority = {O,1,A,L,F,epoch,target,generation,plan})
t2  takeoverLease            commit: high-water F+1, control_leases row -> lease L'
                             control_transitions 'takeover'
                             dispatches(O,1) outcome='transport_unknown'
                             O=reconciling rev=R2
t3  recordDeliveryOutcome(L, R1, P)   REFUSED
```

At `t3` four independent guards are already red: the presented lease `L` is not
the live row (predicate 2), the report's fence `F` is not the live fence
(predicate 3 against the live row via predicate 2), the `authority_decisions`
join still matches but `delivery_outcome IS NULL` no longer holds (predicate 5),
and `expectedRevision` `R1` is not `R2` (predicate 6). Only the first is *the*
answer; the others are why the schedule has no gap.

What happened physically at `t1` is not recoverable: a click that reached the
target cannot be un-clicked. The contract's answer is the one the frozen
authority already specifies — executable specification resolution 1 of
`2026-08-09-runtime-hardening-rewrite.md`: the Operation goes to `reconciling`
with its mutation chain still locked, and only reconciliation may establish a
business terminal disposition. `transport_unknown` is the honest record, and
because it is not `not_delivered`, `commitReconciliation` will refuse a
`Rejected` disposition for `O` (§3.3). This is the whole of `a07`: the takeover
does not prevent the in-flight effect, it prevents the ledger from ever claiming
the effect did not happen.

The reverse schedule — `recordDeliveryOutcome` commits at `t2`, `takeoverLease`
at `t3` — leaves the recorded outcome untouched, because the resolution's
`WHERE delivery_outcome IS NULL` finds nothing and `resolvedDispatches` is `0`.

Cross-process honesty: if the displaced controller runs in another process, its
`TaskHost` never learns the new fence and can still post input. Nothing in a
design that forbids a distributed lease can prevent that. What is prevented, in
every case, is the ledger recording it. Within one process,
`adoptControlFence` closes the physical half too, and §7 T-8 proves it.

## 5. Header-level declarations and every changed signature

### 5.1 New file `modules/task/source/task/host-delivery.hpp`

```cpp
#pragma once

#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <engine/session.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace uf::task
{
    class TaskHost;

    // The control fence one ledger currently holds over one target. Plain data:
    // raising a Host's fence only ever withdraws authority, so a forged value
    // costs its forger and nobody else, and a lower one is refused outright.
    struct ControlFence final
    {
        std::string controlledTargetKey{};
        uint64      sessionEpoch{};
        uint64      fencingToken{};
    };

    // One dispatch the ledger reserved, as the ledger recorded it. The Host
    // checks the four fields it can know -- target, generation, epoch, fence --
    // and carries the rest back untouched so the ledger can recognise its own
    // reservation. Nothing here is proof; the proof is that a
    // HostDeliveryReport exists at all.
    struct DispatchAuthority final
    {
        std::string controlledTargetKey{};
        std::string leaseId{};
        std::string operationId{};
        std::string authorityDecisionId{};
        ContentHash frozenPlanHash;
        uint64      targetGeneration{};
        uint64      sessionEpoch{};
        uint64      fencingToken{};
        uint64      dispatchSequence{};
    };

    enum class DeliveryOutcome : uint8
    {
        // The Host consumed the authorization and posted nothing. The only
        // value that proves an external effect ABSENT.
        NotDelivered,
        Delivered,
        // The Host called the sink and cannot say whether input reached the
        // target.
        TransportUnknown,
    };

    // What one TaskHost::deliver did with one Receipt. Constructible only by
    // TaskHost, so a ledger that demands one cannot be told about a delivery
    // that never ran. Copyable because the Operator stores it by value.
    class HostDeliveryReport final
    {
        friend class TaskHost;

        DispatchAuthority                 m_authority;
        DeliveryOutcome                   m_outcome;
        std::string                       m_reason;
        uint64                            m_receiptId;
        std::optional<engine::ActReceipt> m_act;

        HostDeliveryReport(
            DispatchAuthority authority,
            DeliveryOutcome outcome,
            std::string reason,
            uint64 receiptId,
            std::optional<engine::ActReceipt> act
        );

    public:
        HostDeliveryReport(HostDeliveryReport const&) = default;
        HostDeliveryReport(HostDeliveryReport&&) noexcept = default;
        auto operator=(HostDeliveryReport const&) -> HostDeliveryReport& = default;
        auto operator=(HostDeliveryReport&&) noexcept -> HostDeliveryReport& = default;
        ~HostDeliveryReport() = default;

        [[nodiscard]]
        auto authority() const noexcept UF_LIFETIME_BOUND -> DispatchAuthority const&;

        [[nodiscard]] auto outcome() const noexcept -> DeliveryOutcome;

        // Empty exactly when outcome() is Delivered.
        [[nodiscard]] auto reason() const noexcept UF_LIFETIME_BOUND -> std::string_view;

        // The opaque ordinal of the one Receipt this report consumed.
        [[nodiscard]] auto receiptId() const noexcept -> uint64;

        // Engaged exactly when outcome() is Delivered.
        [[nodiscard]]
        auto act() const noexcept UF_LIFETIME_BOUND
            -> std::optional<engine::ActReceipt> const&;
    };
}
```

### 5.2 `modules/task/source/task/task-host.hpp`

Removed outright — no alias, no deprecation:

```cpp
// BEFORE (private, lines 126-130 and 248)
struct DeliveryAuthority final
{
    uint64 hostNonce{};
    uint64 fence{};
};
[[nodiscard]] auto deliveryAuthority() const noexcept -> DeliveryAuthority;

// BEFORE (private, line 263)
[[nodiscard]] auto takeover() -> Status;

// BEFORE (member, line 205)
uint64 m_fence{1};
```

Changed and added, all private, so nothing production-reachable grows:

```cpp
// BEFORE (line 190, in PendingReceipt)
uint64 fence{};
// AFTER
uint64 fencingToken{};

// BEFORE (line 257)
[[nodiscard]]
auto deliver(
    DeliveryAuthority authority,
    Receipt const& receipt,
    TaskContext& context
) -> Result<engine::ActReceipt>;
// AFTER
// The context is supplied at delivery rather than remembered from minting; see
// the note this replaces. What changed is the return: a refusal that has
// already consumed the Receipt is a FACT about the world, not an error, and the
// ledger cannot record what it is not told.
[[nodiscard]]
auto deliver(
    DispatchAuthority authority,
    Receipt const& receipt,
    TaskContext& context
) -> Result<HostDeliveryReport>;

// AFTER (new, replaces takeover())
// Raises this Host's control fence to the one the ledger now holds. Strictly
// monotone: a fence at or below the current one is refused, so a stale lease
// cannot re-arm a Host a takeover already fenced out. The first adoption also
// binds this Host to one controlled target; a later fence for a different
// target is refused.
[[nodiscard]] auto adoptControlFence(ControlFence fence) -> Status;

// AFTER (member, replaces m_fence)
ControlFence m_fence{};   // fencingToken 0 until one is adopted
```

`mintClickReceipt` gains one refusal at its head: `m_fence.fencingToken == 0`
fails `ActionRejected` with "Host has no control fence to mint against". A Host
the Operator has never authorized can no longer mint, which is a second,
independent reason production cannot act.

### 5.3 `modules/operator/source/operator/ledger.hpp`

```cpp
// BEFORE (lines 123-128) -- deleted from this header
enum class DeliveryOutcome : uint8
{
    NotDelivered,
    Delivered,
    TransportUnknown,
};
```

It moves to `task` (§5.1) because the Host is what produces it. Operator code
and tests spell it `task::DeliveryOutcome`; there is no alias.

```cpp
// AFTER (new)
// What one human takeover did: the lease the new controller now holds, and the
// dispatches this takeover found unanswered and resolved to transport_unknown.
// The count is returned rather than logged because "nothing was in flight" and
// "one effect may already have landed" are different situations for the caller.
struct ControlTakeover final
{
    ControlLease lease;
    uint64       resolvedDispatches{};
};

// The fence a Host must adopt to act under this lease. Derived, never stored:
// one lease has one fence.
[[nodiscard]]
auto controlFence(ControlLease const& lease) -> task::ControlFence;
```

```cpp
// BEFORE (line 243)
[[nodiscard]]
auto takeoverLease(
    std::string const& sessionId,
    std::string const& reason
) -> Result<ControlLease>;
// AFTER
[[nodiscard]]
auto takeoverLease(
    std::string const& sessionId,
    std::string const& reason
) -> Result<ControlTakeover>;

// BEFORE (line 288)
[[nodiscard]]
auto recordDeliveryOutcome(
    std::string const& operationId,
    uint64 dispatchSequence,
    uint64 expectedRevision,
    DeliveryOutcome outcome
) -> Result<StoredOperation>;
// AFTER
// The Operation and the dispatch are read out of the report, because the only
// dispatch this call may answer for is the one the Host was authorized to
// perform. expectedRevision stays a parameter: it is the caller's own read of
// the ledger, not the Host's.
[[nodiscard]]
auto recordDeliveryOutcome(
    ControlLease const& lease,
    uint64 expectedRevision,
    task::HostDeliveryReport const& report
) -> Result<StoredOperation>;
```

`reserveDispatch` returns the authority instead of a bare reservation:

```cpp
// BEFORE (lines 117-121)
struct DispatchReservation final
{
    uint64 dispatchSequence{};
    uint64 operationRevision{};
};
// AFTER
struct DispatchReservation final
{
    task::DispatchAuthority authority;
    uint64                  operationRevision{};
};
```

`dispatchSequence` is not duplicated on the reservation; it lives on
`authority.dispatchSequence`. See §8 for how this meets W2.

`ledger.hpp` gains `#include <task/host-delivery.hpp>`.

### 5.4 Call sites that must change

| File | Site | Change |
|---|---|---|
| `modules/operator/source/operator/ledger.cpp` | `deliveryOutcomeWireName` (line 780) | switches over `task::DeliveryOutcome` |
| `modules/operator/source/operator/ledger.cpp` | `recoverUncertainDispatches` (1539) | body becomes a call to `resolveUnansweredDispatches(db, "", "operator restart found this dispatch unanswered")` |
| `modules/operator/source/operator/ledger.cpp` | `takeoverLease` (2270) | resolves in-flight dispatches; returns `ControlTakeover` |
| `modules/operator/source/operator/ledger.cpp` | `reserveDispatch` (2971) | returns the minted `task::DispatchAuthority` |
| `modules/operator/source/operator/ledger.cpp` | `recordDeliveryOutcome` (3244) | new signature, new predicate, writes `delivery_reason` |
| `modules/operator/source/operator/ledger.cpp` | `verifyExactDatabaseSchema` (337) | expected fingerprint recomputed (§6) |
| `modules/task/source/task/task-host.cpp` | `mintClickReceipt` (528) | fence guard; `PendingReceipt.fencingToken` |
| `modules/task/source/task/task-host.cpp` | `deliveryAuthority` (613), `takeover` (684) | deleted / replaced by `adoptControlFence` |
| `modules/task/source/task/task-host.cpp` | `deliver` (618) | new signature and classification |
| `tests/task/test-runtime-v2-contract.cpp` | `TaskHostTestAccess::deliver` (104) | forwards a `DispatchAuthority` the caller supplies; add `adoptControlFence` and `fence` accessors |
| `tests/task/test-runtime-v2-contract.cpp` | `contract-runtime-u06` (676) | adopt a fence before minting; the wrong-context delivery now asserts `Ok(NotDelivered)` and `clicks() == 0` instead of `CHECK_FALSE(...has_value())` |
| `tests/operator/project-fixture.hpp` | `reconcilingOperation` (835) | cannot fabricate an outcome; drives `reconciling` through `takeoverLease` (§7 note) |
| `tests/operator/test-control-contract.cpp` | `c01` (152), `c09` (402), `c11` (473), `c12` (655) | `takeover->lease.fencingToken`; outcome recording replaced |
| `tests/operator/test-ledger.cpp` | 618, 756, 765, 855, 1133 | same two changes |
| `tests/operator/test-state-contract.cpp` | 231, 338 | same two changes |
| `tests/operator/test-agent-audit-contract.cpp` | 236 | same |
| `conformance/source/suite-support.cpp` | `reconcilingOperation` (302-334) | same as the in-repo fixture |
| `conformance/source/suite-control-ledger.cpp` | 28, 97, 293 | same |
| `tests/CMakeLists.txt` | `test-contract-operator` (300-345) | add `${PROJECT_NAME}_image` and the shared Host fixture source |

The exported conformance suite (`conformance/`) is a separate consumer of this
API and is not optional: `cmake/conformance-suite.cmake` lists its sources
concretely and `tests/CMakeLists.txt:352` includes it, so a missed call site is
a configure-time or compile-time failure, not a silent skip.

## 6. DDL

One table changes. `modules/operator/source/operator/ledger.cpp:690-699`:

```sql
-- BEFORE
CREATE TABLE IF NOT EXISTS dispatches(
    operation_id TEXT NOT NULL REFERENCES operations(operation_id),
    dispatch_sequence INTEGER NOT NULL CHECK(dispatch_sequence > 0),
    decision_basis_hash TEXT NOT NULL,
    frozen_plan_hash TEXT NOT NULL,
    authority_decision_id TEXT NOT NULL
        REFERENCES authority_decisions(authority_decision_id),
    delivery_outcome TEXT,
    PRIMARY KEY(operation_id, dispatch_sequence)
) STRICT;

-- AFTER
CREATE TABLE IF NOT EXISTS dispatches(
    operation_id TEXT NOT NULL REFERENCES operations(operation_id),
    dispatch_sequence INTEGER NOT NULL CHECK(dispatch_sequence > 0),
    decision_basis_hash TEXT NOT NULL,
    frozen_plan_hash TEXT NOT NULL,
    authority_decision_id TEXT NOT NULL
        REFERENCES authority_decisions(authority_decision_id),
    delivery_outcome TEXT
        CHECK(delivery_outcome IN ('not_delivered', 'delivered', 'transport_unknown')),
    delivery_reason TEXT,
    CHECK(
        (delivery_outcome IS NULL AND delivery_reason IS NULL)
        OR (delivery_outcome = 'delivered' AND delivery_reason IS NULL)
        OR (delivery_outcome IN ('not_delivered', 'transport_unknown')
            AND delivery_reason IS NOT NULL)
    ),
    PRIMARY KEY(operation_id, dispatch_sequence)
) STRICT;
```

Why both parts. The vocabulary `CHECK` makes "`not_delivered` is one of exactly
three spellings" a database fact instead of a C++ string comparison, which is
what `commitReconciliation`'s `delivery_outcome<>'not_delivered'` silently
depends on today. `delivery_reason` closes a real gap: the OP schema's
`DeliveryOutcome` requires `reason` for `not_delivered` and `transport_unknown`
(`schema/umbraflow-operator-v1.schema.json`), and the table drops it, so the
Host's reason for refusing to act is currently discarded.

No new table, so the `expectedTables` list (`ledger.cpp:476-482`) is unchanged.
`schema/umbraflow-operator-v1.schema.json` is unchanged, so
`SCHEMA_AUTHORITIES` in `tests/test-runtime-surface.py` and migration-report
stop condition 2 are untouched.

**The fingerprint that must be recomputed** is the expected hash in
`verifyExactDatabaseSchema`, `modules/operator/source/operator/ledger.cpp`.

> **Corrected 2026-08-11 against the landed tree** (`4b955de`, `848e390`). Two
> of the three claims below have been overtaken. The literal is no longer
> unnamed: `848e390` promoted it to
> `constexpr auto k_exactSchemaV1Fingerprint` in the anonymous namespace, which
> is the change this section assigned to W4, so W4 inherits it rather than
> making it. And the value is now
> `sha256:12f64bfff305c30c716fbd5bdc9934a17140dfe4e127b5bce2ec7a10ecd309e4`
> over 20 tables. What still holds is everything that matters here: W4 adds no
> table, so `expectedTables` is unchanged, and the fingerprint must still be
> recomputed because the `dispatches` DDL text changes.

It is the sha256 of the
length-prefixed `type|name|tbl_name|sql` rows of `sqlite_schema` ordered by
`(type, name)`; there is no way to compute it by hand from the DDL text, so the
recipe is: add `actual.hex()` to the mismatch message under a `[DEBUG-w4]` tag,
run one operator test, copy the value, remove the tag. `initialize()` calls
`verifyExactDatabaseSchema` immediately after creating the schema
(`ledger.cpp:769`), so a stale constant makes *every* Operator open fail — a
forgotten recomputation cannot ship green.

`PRAGMA user_version` stays `1`. The fingerprint is the database's identity;
`user_version` and `application_id` are the cheap pre-filter that separates "an
Operator database" from "some other SQLite file". Nothing is released and no
file is migrated, so a second version number would be a field that never gets
read. See open question Q2.

## 7. Falsifiable tests

Every row names the property, where the case lives, and the single-line source
mutation that must turn it red. A mutation that would leave the case green is
called out as such rather than listed.

Two of these need a real `TaskHost`. The delivering fixture already exists in
`tests/task/test-runtime-v2-contract.cpp` (the Runtime v2 artifact, the fake
`FrameSource`/`ActionSink`, `RuntimeContext`, `loadedRuntime`,
`k_authorizeSource`, and `TaskHostTestAccess`). W4 extracts it into
`tests/support/runtime-host-fixture.{hpp,cpp}` and both conformance suites use it;
copying it would be the second spelling of one thing. This extraction, not the
ledger work, is the reason W4 is larger than the one-line "3 days" in
`2026-08-10-next-block.md` §3.

| # | Property | Case | Mutation that must turn it red |
|---|---|---|---|
| T-1 | A Host report records, and the recorded row is the dispatch the report names | `contract-control-c03` | in `recordDeliveryOutcome`, bind a literal `"other-operation"` in place of `report.authority().operationId` |
| T-2 | The outcome lands on the dispatch sequence the report names, not on the first one | `contract-control-c03` (two dispatches on one Operation) | bind a literal `1` in place of `report.authority().dispatchSequence` |
| T-3 | A report authorized by one lease cannot be recorded under a different lease value | `contract-control-c03` (copy `prepared.lease`, change `leaseId`) | delete `AND lease.lease_id=?5` from the lookup |
| T-4 | A report cannot be recorded once the fencing token has moved | `contract-control-c03` (copy `prepared.lease`, `fencingToken + 1`) | delete `AND lease.fencing_token=?6` |
| T-5 | The report's own lease identity must match the presented lease | `contract-control-c03` (deliver with `authority.leaseId = "other-lease"`) | delete the `authority.leaseId != lease.leaseId` clause from the C++ pre-check |
| T-6 | A report naming another authority decision or another frozen plan is refused | `contract-control-c03` (deliver with a mutated `authorityDecisionId`, then a mutated `frozenPlanHash`) | delete `AND a.authority_decision_id=?8`; separately delete `AND d.frozen_plan_hash=?9` |
| T-7 | The Host refuses an authority naming another target, generation or epoch, and posts nothing | `contract-control-c03` | in `TaskHost::deliver`, delete `\|\| authority.controlledTargetKey != m_fence.controlledTargetKey` (then each of the generation and epoch comparisons in turn) |
| T-8 | One Receipt authorizes exactly one report | `contract-control-c03` | in `TaskHost::deliver`, delete `m_receipts.erase(found);` |
| T-9 | **In-flight takeover.** Deliver, then take over, then try to record: the record is refused, the dispatch is `transport_unknown`, the Operation is `reconciling`, and `takeover->resolvedDispatches == 1` | `contract-agent-a07` | in `takeoverLease`, delete the `resolveUnansweredDispatches` call. *Note:* deleting the lease predicate from `recordDeliveryOutcome` leaves T-9 green, because the outcome CAS also refuses — that predicate is falsified by T-3 and T-4, which is why they are separate cases. |
| T-10 | Reverse order: record, then take over — the recorded outcome survives and `resolvedDispatches == 0` | `contract-agent-a07` | drop `AND delivery_outcome IS NULL` from the resolution `UPDATE` |
| T-11 | Only `not_delivered` unlocks `Rejected`: a `NotDelivered` report allows it, a `TransportUnknown` report does not | `tests/operator/test-ledger.cpp` | in `commitReconciliation`, change `delivery_outcome<>'not_delivered'` to `delivery_outcome<>'delivered'` |
| T-12 | Nothing outside `TaskHost` can make a report | `tests/operator/test-ledger.cpp`, `static_assert(!std::is_default_constructible_v<task::HostDeliveryReport>)` and `static_assert(!std::is_constructible_v<task::HostDeliveryReport, task::DispatchAuthority, task::DeliveryOutcome, std::string, uint64, std::optional<engine::ActReceipt>>)` | make `HostDeliveryReport`'s constructor `public` |
| T-13 | A Host with no adopted fence cannot mint | `tests/task` suite via the shared fixture | delete the `m_fence.fencingToken == 0` guard from `mintClickReceipt` |
| T-14 | After adopting a takeover fence, a Receipt minted under the old fence is `NotDelivered` and no click is posted | `tests/task` suite via the shared fixture | in `TaskHost::deliver`, delete `pending.fencingToken != m_fence.fencingToken` from the post-consumption check |
| T-15 | `adoptControlFence` is strictly monotone | `tests/task` suite | change `<=` to `<` in the monotonicity guard |
| T-16 | A refused cycle is reported, not clicked | `contract-runtime-u06` | delete the `UF_TRY(context.requireReceiptCycle(...))` guard in `deliver`; the case's `clicks() == 0` assertion goes red |

`tests/test-runtime-surface.py` keeps passing without modification: the ten
`HOST_VALIDATION_TEST(DeliveryAuthority.*)` markers stay in
`tests/operator/test-control-contract.cpp`, which `tests/CMakeLists.txt:306`
names, and `receipt_validation_errors` checks only that each schema field has a
marker in a registered source. W4 moves each marker next to the assertion that
now validates that field, so the markers stop being decoration. Do not add a
non-`contract-*` `TEST_CASE` to that file: `cpp_add_contract_suite` registers
the binary `NO_CTEST` and only exports per-case CTest entries, so such a case
would never run. Extra behavioural cases go in `tests/operator/test-ledger.cpp`,
which is a CTest target in its own right.

## 8. Assumptions about W2 and W3

W2 and W3 are being specified in parallel. These are the assumptions W4 makes;
each is a reconciliation point, not a claim about what those documents say.

About W2 (`docs/plans/2026-08-10-w2-effective-plan.md`):

- **A1.** `reserveDispatch` stays the single mint of dispatch authority. W4
  requires it to *return* a `task::DispatchAuthority`; if W2 keeps returning a
  bare `DispatchReservation`, W4 adds the authority field to it. Either way no
  caller composes an authority for a real dispatch.
- **A2.** W2 changes `reserveDispatch`'s *parameters* (an `EffectivePlan`
  instead of three caller hashes). W4 changes only its return type and adds
  nothing to its parameters, so the two edits do not overlap textually.
- **A3.** `authority_decisions` keeps `authority_decision_id`, `lease_id`,
  `session_epoch`, `fencing_token` and `dispatch_sequence`, and `dispatches`
  keeps `frozen_plan_hash`. W4's fence predicate reads exactly these.
- **A4.** W2 does not change the `dispatches.delivery_outcome` vocabulary or
  `DeliveryOutcome`'s three values.
- **A5.** W2's bounded step sequencing keeps "at most one unanswered dispatch
  per Operation" — today enforced by `reserveDispatch`'s
  `sqlite3_column_type(query.get(), 4) == SQLITE_NULL` refusal
  (`ledger.cpp:3101-3107`). If W2 allows several in flight,
  `resolveUnansweredDispatches` must resolve every NULL row rather than the
  latest, and T-9's `resolvedDispatches == 1` becomes a count.
- **A6.** If W2 also changes the DDL, whichever of W2 and W4 lands second
  recomputes the schema fingerprint. Both changing it independently is a merge
  conflict on one literal, which is the intended failure mode.
- **A7.** W4 should land **after** W2. The dependency is real but narrow: it is
  only `reserveDispatch`'s final shape.

About W3 (`docs/plans/2026-08-10-w3-snapshot-coordinator.md`):

- **A8.** W3 does not change `ControlLease`, `control_leases`,
  `fencing_high_water` or `control_transitions`. W4's fence reads only those.
- **A9.** W3 does not introduce a seventh operation that mutates control state
  outside a `BEGIN IMMEDIATE` transaction. §1.2's claim that five of six
  operations are serialized by SQLite must be re-checked against W3's
  `ProjectObservation` composition before W4 is accepted.
- **A10.** A takeover need not delete snapshot rows: `createSnapshot` already
  refuses a superseded lease (`ledger.cpp:2554-2571`) and W3 keeps that.
- **A11.** W3 does not require `OperatorCoordinator` to hold or call a
  `TaskHost`. If snapshot composition needs live UI observation through the
  Host, the friend problem of §2 returns and W4's boundary answer must be
  revisited before W3 lands.
- **A12.** W4 has no hard code dependency on W3. `2026-08-10-next-block.md` §3
  lists W3 as a W4 prerequisite; as specified here it is a sequencing
  preference, not a blocker.

## 9. Open questions

- **Q1.** `EngineSession::clickPoint` cannot distinguish a refusal before the
  sink from a trace-emit failure after the click landed, so §4.2 maps every
  click-path error to `transport_unknown`. That under-claims: `beginDelivery`
  and `authorizeCoordinate` failures really are proven absences. Should the
  engine return a delivery-phase result so those become `not_delivered`? It
  widens how often `Rejected` is reachable in reconciliation. Out of W4's scope
  as written; it changes a `KEEP` module.
- **Q2.** `PRAGMA user_version` stays `1` across a schema-byte change (§6). Is
  the fingerprint the sole identity, or should `user_version` track schema
  revisions so a mismatch reports "wrong revision" rather than "wrong bytes"?
- **Q3.** `releaseLease` is untouched. A controller may therefore release its
  lease with a dispatch still unanswered, leaving a NULL outcome until the next
  restart's `recoverUncertainDispatches`. Should `releaseLease` refuse while a
  dispatch is in flight, or resolve it the way a takeover does? A release is
  voluntary and a takeover is not, which argues for refusing — but that is a new
  rule, not a W4 consequence.
- **Q4.** The exported conformance suite loses the ability to assert `Delivered`,
  because only a Host can produce that value and the suite has no Host. §7 keeps
  its coverage by driving `reconciling` through a takeover instead. Is that
  enough for `P-05`, or must the exported suite grow a Host fixture — which
  would make the Runtime v2 artifact part of the consumer-visible surface?
- **Q5.** `adoptControlFence` is private and reachable only from the test
  harness, so nothing in production ever adopts a fence. W6's controller facade
  is the natural owner. Until then the Host half of `a07` is proved by tests
  and exercised by nothing else, which is correct but worth stating in the
  requirement matrix rather than leaving implied.
  > **Carried 2026-08-11 (`07abc3e`), twenty hours late — and then misapplied.**
  > This question was right: W6 landed without wiring `adoptControlFence` onto a
  > production path, and the matrix row was marked closed with no caveat. The
  > carry reached the requirement matrix, exactly as asked, but the matrix used
  > it to declare `a07`'s first acceptance clause unimplemented. It is not: that
  > clause is `reserveDispatch`'s live-lease predicate, closed at the ledger
  > layer with no Host involved at all. What Q5 actually names — no production
  > code adopts a fence, so the Host side of the takeover/delivery join is
  > proved by tests and reachable by nothing else — is real, is still true, and
  > is [F-4 in the cross-repository drift review](2026-08-11-cross-repository-drift.md),
  > a deliberate divergence from the bundle's design rather than a gap in `a07`.
  > The true gap in `a07` was narrower than either reading: no case ran the
  > schedule of taking over and then attempting a reservation on the displaced
  > lease. `contract-agent-a07` was extended to run it and closed `a07` again on
  > 2026-08-11 (`bed456f`); see
  > [the next block](2026-08-10-next-block.md) §2. Q5 is still the worked
  > example behind the rule proposed in §6.1 there — an open question that names
  > a requirement blocks that requirement's row — but the second lesson is that
  > the concession must also name the acceptance clause it threatens, or the
  > next reader re-scopes it by guess, which is what happened here.
- **Q6.** §5.2 binds one `TaskHost` to one `controlled_target_key` at its first
  fence adoption. Is that the intended shape, or will one Host serve several
  targets — in which case `m_fence` becomes a per-target map and every
  Receipt carries its target.
- **Q7.** This document is not yet linked from `docs/plans/README.md` or
  `docs/INDEX.md`. Both entries are owed when W4 is accepted.
