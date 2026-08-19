# 2026-08-17 — The interface-lock parity gate is registered, and takes the lock by path

## Decision

`tests/contracts/test_interface_lock_parity.py` is registered as
`check-interface-lock-parity`. It compares this repository's producer schemas
against the bytes the consumer froze in its interface lock.

It finds the lock through the CMake cache entry `UF_CONSUMER_INTERFACE_LOCK`, and
this repository declares no default for it. Undeclared, the gate is registered and
reports itself skipped **by name**. Declared, it carries the `CI` label.

## Context

The parity test had been written and registered in no CTest target: it had never
run outside a hand invocation. That is the same defect class as a hash nothing
compares — a check that exists and cannot fail.

Two details were ruled deliberately.

The path is a **cache entry and not an environment variable**, for the reason
`CONTEXT.md` gives for `project --frames-root`: nothing here reads the
environment, and a run whose outcome depends on ambient state has no record of
what produced it.

There is **no default path**, so the defect that
[2026-08-16](2026-08-16-no-exact-byte-consumer-bundle-pin.md) removed cannot come
back: no gate registered by this repository may REQUIRE another repository.
Registering-and-skipping-by-name rather than silently vanishing is what keeps the
absence visible, and the `CI` label when declared is what keeps the result read —
a divergence report nobody is required to read is the defect the unregistered
test already was.

## Consequences

- The first run of this gate printed a real disagreement rather than a green
  light. The disagreement itself is not ruled on here: which side is right is
  owed by whoever next revises the interface lock, and the current divergence set
  belongs to the consumer's execution ledger, not to any document in this
  repository.
- Contract-version agreement between the two repositories remains a review
  obligation outside what the lock's vectors cover.
