# 2026-08-15 — The HostPlugin architecture proposal: A approved, B refused, C not considered

## Decision

Option A was approved, widened by a second identity defect found during the
review, and is implemented. Option B was refused. Option C was not considered.
`ARCHITECTURE.md`'s deliberate-absence sentence stands unchanged: no dynamic
plugin ABI, no dependency-injection container, no message bus.

## Context

The proposal argued for a generic Host-side plugin kernel. Option B was sold on
the evidence it would purchase, and that evidence can be read today without it —
so the purchase price bought nothing that is not already in hand. Option A was
the narrow fix, and reviewing it turned up a second identity defect of the same
kind, which was folded into the approved scope rather than deferred.

The review document is archived at
[`docs/archive/reviews/2026-08-14-host-plugin-architecture-proposal.md`](../archive/reviews/2026-08-14-host-plugin-architecture-proposal.md)
and is a ruled historical review, not a live proposal.

## Consequences

- One obligation outlived the review: hoist `modules/cli/source/cli/platform/`
  into a shared module when a second real assembly root exists. It is owned by
  the product roadmap, not by this file.
- The lifecycle work that might have been read as adopting the proposal was ruled
  separately and explicitly did not adopt it — see
  [2026-08-17 — lifecycle ownership is explicit composition, not a kernel](2026-08-17-runtime-lifetime-boundaries.md).
