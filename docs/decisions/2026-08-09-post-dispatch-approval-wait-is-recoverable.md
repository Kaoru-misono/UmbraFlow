# 2026-08-09 — Post-dispatch approval wait is recoverable

## Decision

The unique Operation transition table gains this fail-closed edge:

```text
awaiting_approval (plan_frozen_at != null) -> reconciling
  guard: cancel, deadline, lease loss, restart epoch change, or approval can no
         longer be obtained
  effect: freeze further input; retain frozen plan and mutation-chain lock
```

It never transitions directly to cancelled or expired, and never releases the
mutation chain. Reconciliation alone may establish a business terminal
disposition.

## Context

One of the four executable specification resolutions: places where the frozen
product contract contained a contradiction, and this repository picked one side
and froze the choice upstream. This is not a second product authority — the edge
closes the consumer main design §8.4 requirement that cancel, deadline or lease
loss *after dispatch* enters reconciliation.

The contradiction is that an approval wait looks like a state you can simply
abandon, while a dispatch has already happened: the world may have changed. Any
edge that ends the operation without reconciling asserts the dispatch had no
effect, which is precisely what nobody knows at that moment.

## Consequences

- Losing a lease during an approval wait is a recoverable condition, not a
  terminal one.
- The frozen plan and the mutation-chain lock survive the transition, so the
  reconciler reads the same plan the dispatch was made from.
- Changing this requires a new consumer bundle contract version; a checked-in
  upstream schema only makes the existing contract executable.
