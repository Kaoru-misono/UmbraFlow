# 2026-08-22 — A join needs two independent sources

## Decision

This ruling fixes where the fact "these are the entries the closure exports"
comes from. It is the producer side of the bind-time refusals frozen in
[`a Tool handler and an automation script are one mechanism`](2026-08-22-handler-and-automation-script-are-one-mechanism.md),
which requires the loader to refuse a binding naming an entry the closure does
not export — a refusal that is only as real as the independence of the two things
it compares.

### Where the statement comes from and what carries it

`exportedEntryPoints` is:

- **produced by the authoring path**, never written by hand;
- **carried by deployment into the registration**;
- **covered by the manifest hash**, so changing it moves registration identity;
- **cross-checked against the binding union at load**; and
- **proven against the closure's actual exports by the bridge at execution.**

Both closures of a registration get one:
[`one job, one vehicle`](2026-08-22-one-job-one-vehicle.md) makes them
`{plugin_id, reduce}` for the reducer closure and `plugin_id` plus the binding
union for the tool closure.

The authoring **tool** computes the statement by running the module in the
author's own sandbox — nothing short of running a Luau module reads its table's
keys, so it runs where the author already runs code — but what ships is a
committed, hashed **statement**, not a re-derivation. Generate it on every
authoring build, so staleness is impossible on the normal path and lying takes
deliberate effort that the bridge then catches.

### The loader neither derives it nor observes it

The loader may not compute `exportedEntryPoints` from the binding table, and may
not execute the module to observe it. Both are refused, and for the same reason
stated in two directions.

### The general rule

> **An admission that joins a claim against a behaviour needs two independent
> sources. Collapsing either source into the other leaves a check no test can
> make fail, and deletes the property the check existed to pin.**

This is the rule to cite whenever a field looks redundant because something
nearby could compute it. Redundancy is the point: the check is the disagreement
between two sources, so a field that cannot disagree is not a cheaper version of
the check — it is the check's absence.

## Context

**What each collapse actually does.** The admission exists to join a *claim*
(what the deployment says the closure exports) against a *behaviour* (what the
closure exports when run).

- *Loader derives from bindings.* The claim is now computed from the binding
  table, so the load-time check compares the binding table to itself. It passes
  unconditionally. The refusal "a binding names an entry the closure does not
  export" still exists in the source and can never fire.
- *Loader executes to observe.* The claim is now computed from the module's
  actual exports, so the bridge compares exports to exports. It passes
  unconditionally, for the mirror-image reason.

Either way the remaining branch is dead — forbidden on its own terms — and the
property that a deployment's stated contract matches its shipped code is no
longer pinned by anything.

**Why loader-side execution is worse than merely vacuous.** It runs untrusted
module top-level code inside the trusted loader. That creates an execution
context with its own environment question, its own quota question, and its own
failure-classification question, in the one component that has none of those
today. The runtime deliberately confines all of them to the dispatch executor,
where a run has a coordinate, a lease, a fence and a durable row to be classified
against. A loader has none of that, so a module that throws, hangs, or allocates
during observation has no classification available and no place to record one.

**The cost, stated plainly rather than argued away.** A stated field can drift
from the code it describes. Drift surfaces as a load-time refusal or a
first-execution refusal, so an artifact can deploy successfully and then refuse to
register. That is a real regression in when the author learns, and it is the
price of having a checkable join at all. It is paid visibly, at the trust
boundary, which is the right place for it: the alternative is paying nothing and
having nothing.

**Why generation rather than authorship is the mitigation.** The failure mode
above is entirely about hand-maintained fields. A statement regenerated on every
authoring build cannot be stale on the normal path; it can only be wrong if
someone edits the generated artifact deliberately, and the bridge's exact-export
admission catches exactly that. So the cost is bounded to the case where the
mitigation is a deliberate act of falsification, which is the case the execution
check exists for.

## Consequences

- `exportedEntryPoints` is a registration member under the manifest hash. A
  proposal to omit it from the hash to avoid churning identity on a refactor is
  refused: an unhashed claim is a claim no pin covers, and re-registration could
  then change what the loader checks against without moving identity.
- No loader-side execution of Project module top-level code, for this or for any
  other observation. If something must be known about a closure's behaviour
  before dispatch, it is stated by the author, hashed, and proven at execution.
- Neither the load-time cross-check nor the bridge's exact-export admission may
  be removed as redundant. They catch different things: the first catches a
  deployment whose declared contract and stated code disagree, the second catches
  a stated code that the shipped bytes do not honour.
- The authoring path generates the statement. A Project Kit flow, template, or
  document that invites an author to type an entry list is a defect in the flow,
  not a convenience.
- When a future field is proposed and someone observes that the loader could
  compute it, the rule above applies before any other consideration. Ask what
  disagreement the field exists to detect; if the answer is "none, once it is
  derived", the field and the check are both being deleted.
