# 2026-08-27 — A call states the screenshot it committed

[`2026-08-26-a-tool-is-a-flat-call-over-an-explicit-reference.md`](2026-08-26-a-tool-is-a-flat-call-over-an-explicit-reference.md)
ruled that a screenshot is an immutable content-addressed artifact, that only
its receipt enters the ledger, and — in Z4 — that `framework.input.hold` may
capture or observe **while still pressed** and answer with what it saw. This
settles how the ledger learns which blob a call committed, because on the terms
that ruling landed on, a hold's frame was committed by nothing.

## The measurement

Driven against a live session on this date:

- `framework.input.hold` with `return_screen = "capture"` answered with
  `screen.screenshot_sha256`, its `width`, `height`, `byte_count` and
  `media_type`. Passing that digest to `framework.screen.read_lines` was refused:
  `screenshot_sha256 <digest> is missing or expired: no committed screenshot
  receipt names a retained evidence blob` — `action_rejected`,
  `terminal_failure`.
- With `return_screen = "observe"`, the result carried no `screenshot_sha256` at
  all, so the frame it resolved on had no name its caller could use.

The declared capability was therefore unreachable in both spellings: a caller
could learn a digest and do nothing with the frame. The consuming project needs
exactly this frame — a long press is how that game opens a card's detail panel,
and the panel exists **only** while the button is held, so a capture after
release cannot show it, and `hold` is synchronous, so a chunk cannot take its
own capture during the press.

## Why it was refused

`OperatorCoordinator::evidenceArtifactReceipt` — the resolver every measuring
Tool goes through, and the retention sweep's reference set — read the receipts
out of one query:

```sql
WHERE history.state='confirmed' AND position.tool_name IN
('framework.screen.capture','framework.screen.crop')
```

and then parsed each such call's **entire outcome payload** as a receipt.

So "a committed receipt" meant "the whole confirmed result of one of two Tools
named in SQL". That rule holds only while every receipt-producing Tool answers
with nothing but a receipt. A hold's receipt is one member of a larger result
and cannot satisfy it, and neither could any future Tool that returns a frame
beside anything else. Both consequences followed at once: the digest resolved to
nothing, and the blob was unreferenced, so the next
`reclaimUnreferencedEvidenceArtifacts` pass would delete it.

## The ruling

**A confirmed Framework Tool call states the screenshot receipt it committed, in
its own evidence, under `screenshot_receipt`. That is how the ledger learns
which blobs a run committed, and no Tool name takes part.**

`framework.screen.capture`, `framework.screen.crop` and both screen-returning
holds attach the receipt they published. The query becomes "a confirmed call,
answered by the Framework, whose evidence states a receipt", and the receipt is
read from that member rather than from the shape a result happens to have.

**And an observation names the frame it resolved.** Every
`framework.screen.observe` result carries `screenshot_sha256`. For a caller who
supplied the digest this is an echo; for a hold that observed while pressed it is
the only name the caller has for a frame the framework captured on its behalf,
and a result that cannot be chained is not a result.

The Luau form the ruling makes work:

```lua
local screen = require("@umbraflow/screen")
local input  = require("@umbraflow/input")

local held = input.hold{
    duration_ms       = 600,
    return_screen     = "capture",
    screenshot_sha256 = screen.capture{}.screenshot_sha256,
    x = 812, y = 455,
}.screen

-- The detail panel is gone from the live screen by now. The frame is not.
local lines = screen.read_lines{
    screenshot_sha256 = held.screenshot_sha256,
    x = 640, y = 180, width = 520, height = 700,
}
```

### Why the evidence column and not a table of its own

The durable channel from a provider to the ledger is `ToolCallCompletion`, whose
whole content is a classification, a payload and an optional evidence document.
Anything else — a receipts table keyed by call identity — is a schema change:
a new DDL constant, a new `k_operatorDatabaseSchemaIdentity`, a new step
appended to all twenty registered migrations, and a wind-back in every migration
fixture. That price buys a column shape; it does not buy a different rule, and
the rule is the whole defect.

Evidence is already the place where a call records what the framework observed
about its own effect — `host_delivery`, `posted_inputs`, `receipt_id` for an
input delivery. "This call published this blob" is the same kind of fact,
recorded beside them in one object rather than instead of them.

### Why not commit at publication

Rejected, and the invariant it would break is named in a test:
*a screenshot receipt is committed only after its blob is durable*. Publication
synchronises and renames the bytes; a call that dies between publication and
completion must leave an orphan the sweep can reclaim, not a retained receipt.
Commitment stays at completion, which is also what keeps replay deterministic —
`replayToolCall` answers a recorded hold from its stored payload and evidence
together, so the digest it hands back on replay is the one whose receipt is
already durable.

### Why not route the hold through a nested screen call

Rejected. Making the hold's frame a real `framework.screen.capture` call would
give it every property for free, and it is exactly the call tree the nesting cut
retired — `removeNestedToolCallSchema`, and the failures recorded in
[`docs/pitfalls/observation-frame-and-ledger-children.md`](../pitfalls/observation-frame-and-ledger-children.md).
Re-entering the executor from inside a provider is not a small step back.

What *was* taken from that idea is the part that costs nothing: the hold no
longer inlines its own copy of the capture and observe bodies. One seam,
`heldScreen`, produces the frame through the same `captureOpenCycle` and the
same `observedOpenCycle` that answer `framework.screen.capture` and
`framework.screen.observe`, so the declaration's promise — "it is a
`framework.screen.capture` receipt or a `framework.screen.observe` result" — is
kept by producing one, not by rendering something shaped like one.

### Why not add the hold's names to the IN-list

Rejected as the shim CLAUDE.md forbids. It would have taught the reader two
shapes of a stored receipt — the whole payload for two Tools, a nested member
for two others — and left the next Tool that returns a frame beside anything
else to discover the same defect a third time.

### The trust boundary is `provider_kind`, not the two names

The retired IN-list kept a Project-answered Tool from claiming a committed blob,
by accident: the two names it listed are reserved. The replacement states that
property instead of relying on it — the query is restricted to
`position.provider_kind='framework'`. A Project provider's evidence is never
read as a receipt.

## Consequences

- One rendering of a receipt exists, `operator_runtime::evidenceReceiptJson`,
  beside the parser that reads it back. `service` no longer keeps a second
  spelling of the same eight members.
- `framework.screen.observe` results gain `screenshot_sha256`, and
  `tool_catalog_hash` moves. Nothing outside this repository pins that value.
- Both hold declarations now say the frame is retained and measurable, because
  a capability a caller cannot discover from the catalog is one an agent does
  not have.
- Retention follows resolution: a held frame is swept on exactly the terms every
  other observation is, because it is now in the same reference set.
