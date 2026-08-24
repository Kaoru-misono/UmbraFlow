# 2026-08-24 — `on_timeout` is enforced, and no input state outlives its Tool call

This settles a debt two earlier decisions recorded, and both keep their bytes:

- [`2026-08-23-the-framework-enforces-limits-it-was-given.md`](2026-08-23-the-framework-enforces-limits-it-was-given.md)
  listed `timeout_policy` under "what does not conform", as published and
  unenforced. That is now false and the debt is paid.
- [`2026-08-23-the-framework-stops-interpreting-project-state.md`](2026-08-23-the-framework-stops-interpreting-project-state.md)
  said the same and deferred repayment to "immediately after the flip". This is
  that repayment.

## What `on_timeout` now does

Enforcement lives in `ToolRuntimeExecutor::invoke`, the one execution seam every
caller adapter passes through. It brackets the provider call, compares elapsed
whole milliseconds against the Tool's declared `maximum_elapsed_ms`, and gives an
overrunning call a durable outcome that **is** the timeout, whatever the provider
went on to answer.

The reported signal is specific, as the governing line demands of every limit:
*«tool» exceeded the maximum_elapsed_ms of «N» its Tool declared: the call
returned after «M» ms*, with `kind`, `maximum_elapsed_ms`, `elapsed_ms`,
`on_timeout` and `failure_response` carried machine-readably beside it.

Enforcement is deliberately **post-hoc**. A provider runs synchronously on the
calling thread, so nothing at that seam can take a running one away from it. The
ceiling decides the call's outcome and whether the run continues — not whether
the provider is interrupted. The alternative would be a framework that kills work
mid-flight, which is a much larger promise than a Project asked for.

## The framework decides whether the run continues; it does not act for the Project

This is the reading `reobserve` and `stop` are given, and it follows from the
governing line rather than from convenience.

- **`stop`** ends the run: `invoke` returns a failure, which is what every
  adapter and the scoped Luau seam already treat as terminal.
- **`reobserve`** keeps the run alive with the timeout as the call's recorded
  answer, so the Project looks again itself.

The framework does **not** mint a `framework.screen.observe` on the Project's
behalf. Doing so would have it perform the Project's next action, which is the
Project's to choose; it would also make the shape of the recorded call tree
depend on a timeout, so what the record says happened would differ from what the
Project asked for. Whether the run continues is a limit the Project declared and
the framework enforces. What to do next is the Project's.

**A naming debt, named and not scheduled.** `reobserve` names an action the
framework does not perform. Under this ruling it means "do not tear the run
down", which would be spelled `continue`. That is the same defect shape as the
value deleted below — a name promising something that does not happen — and it is
recorded here rather than fixed, because renaming it is a second published
vocabulary change in the same area and deserves its own decision.

## `reconcile` is deleted, and the reason is not the one first supposed

The dispatching brief asserted that reconciliation had been deleted with the
Project-state cut. **That was wrong**, and the correction matters: `78cb57d`
deleted the Project-state interpretation — Journal, reducer, revision, final
compare-and-swap. **Tool-call reconciliation is alive**: `ToolCallReconciliation`,
`ToolCallReconciliationKind`, `ToolReconciliationQuery` and
`OperatorCoordinator::reconcileMutatingToolCall` all exist and are published
vocabulary.

The value still goes, for a stronger reason. Reconciliation is a transition only
a call already classified `possible` may take, and it needs a trusted external
`ToolReconciliationQuery` that **has no production producer anywhere** — only
tests call it. A timed-out mutating leaf is already classified `possible` by the
existing rule whatever `on_timeout` says, and the framework has no reconciler to
invoke. So no framework behaviour could distinguish `reconcile` from the other
two values: **a Project choosing it would be silently given something else.**

That is worse than an absent value, which is why it is deleted rather than left:
the enum, its wire name, its parse entry, the deployment reader's accepted array,
both published schema enums, the two Framework input descriptors that carried it,
and the tests that only proved it. This is a published-contract change.

A sibling vocabulary carries the same shape of debt and is **not** touched here:
`ExternalInputAction::FreezeAndReobserve` / `FreezeAndReconcile` are likewise
unenforced with no production producer. Different vocabulary, same question,
separate decision.

## A collision the enforcement exposed

`framework.workflow.wait` declared its timeout ceiling equal to the longest wait
it accepts, and the script-facing discovery entry published **that same number**
as the `maximum_duration_ms` bounding `wait()`. Enforcing the ceiling would
therefore have timed out every maximal legal wait — the Tool would have been
declared to fail at exactly the value it advertised as allowed.

The two numbers are separated: discovery publishes the workflow budget a stated
duration must lie within, and the Tool's own timeout ceiling gains headroom above
it, with the reason written at the constant. A Framework Tool choosing headroom
for itself is the Framework declaring its own limit, not inventing a Project's.

Consequence, stated because it is a pinned identity: **the Framework Tool
Catalog hash moves.**

## No input state may survive the end of its Tool call

A new invariant, and it belongs to the safety of the flow rather than to any
Project's decision:

> On timeout, on error, on abort, the framework unconditionally releases every
> input still held.

It is the same class as refusing an unauthorised actor. The framework is not
deciding what the Project should do; it is refusing to leave the machine in a
state nobody asked for. **A hung call still holding a mouse button down is a
worse failure than a hung call.**

The primitive is `IActionSink::releaseHeldInputs`, and the unconditional call
site is `EngineSession::endDelivery`, reached by all five sink-calling paths —
key, scroll, click, long press, pointer move and drag — on success as well as
failure. A failed release folds into the delivery's error as context, or is
reported when the delivery itself succeeded.

`ControllerActionSink::drainAfterFailure` and the per-verb port clauses obliging
each implementation to leave the button released are **deleted**. Keeping both
would be two spellings of one guarantee, and the weaker spelling was the one that
relied on every adapter remembering. The guarantee is now the framework's.

This lands before the `hold` action exists, deliberately: `hold` will press, make
child observations and release, and its safety rests entirely on this invariant.
It was falsifiable before `hold` because `drag` already holds a button for the
whole of its travel, so the leak was reachable today — the falsification aborts a
drag mid-travel and watches the button stay down when the teardown is removed.

## Left alone, and why

`docs/PUBLIC-CONTRACT.md` does not cover `on_timeout` at all — the generator
never emitted it, so the deleted value's published home was the two JSON schemas,
where the deletion landed. The contract check's no-op is correct rather than a
miss. Whether the public contract should carry this vocabulary is a separate
question.

`brief-timeout` in the shared test fixtures is a Tool with no consumer anywhere,
left from deleted step-intent generation. Changing it would move the shared
fixture's registration bytes for no gain here.
