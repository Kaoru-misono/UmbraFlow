# 2026-08-17 — Lifecycle ownership is explicit composition, not a kernel

## Decision

The lifecycle-ownership review hardened the existing explicit composition rather
than adopting a Host kernel. It did **not** approve the archived HostPlugin
proposal.

The boundaries it fixed:

- `service::ProductLifecycle` exclusively owns the production `OperatorTaskHost`,
  control lease, controller binding, runtime generation and plan authority. All
  recoverable setup that can refuse completes before the lease is acquired. Its
  heap-resident `Impl` is constructed first and stores the acquired lease directly
  as optional active state, so acquisition leaves no fallible ownership transfer.
  `shutdown()` is idempotent and is the reporting path; `cli::observeProject`
  calls it unconditionally and combines its failure with the work result through
  `reportAfterClose`.
- A live `ProductLifecycle` is move-constructible but **not** move-assignable.
  `ProductLifecycle::Impl` releases any remaining active lease as a last-resort
  RAII fallback, which does not replace the explicit close path.
- `TaskHost` owns every Runtime or Annotation generation. `cancel()` requests
  stop.
- `EngineSession` owns its frame, action and optional OCR providers but borrows
  one stable `TraceRecorder`. `ExplorationSession` makes that relation safe by
  owning the recorder before the context and VM, keeping it behind `unique_ptr`
  and forbidding moves.
- `ExplorationSession::finish()` is the reporting close for `run.finished`; its
  destructor is structural cleanup only.

## Context

The proposal on the table was a HostPlugin kernel that would own these
relationships generically. Measured against what exists, the shared composition
was two functions organizing roughly three hundred lines, and a shell that
organizes a single-implementation factory collides with its own stop condition.
The genuine defects the review found were narrower and did not need a kernel:
move assignment on a live lifecycle could overwrite an active lease with no return
channel for a close failure, and the recorder-borrow relation was safe only by
declaration order.

## Consequences

- `OperatorTaskHost` serializes lease acquire, release and takeover with dispatch,
  and rolls acquisition back if the Host cannot adopt its fence. The next Operator
  open invalidates process-local lease state, but a failed RAII fallback can still
  omit the expected release transition from the audit trail. A destructor cannot
  report that failure.
- There is no generation retirement or quiescence operation. That is sufficient
  while the Host dies with one `ProductLifecycle`; it is not sufficient for a
  resident Host that reloads generations in place.
- The sole production loop reaches `finish()` on one exit path, but the type does
  not yet enforce that protocol if another caller is added.
- Other composition roots must preserve the documented declaration order until a
  single aggregate owns the recorder relationship.
- The thresholds that must be ruled on and tested before a resident Host or a
  second real assembly root exists are owned by the product roadmap, not by this
  file.
