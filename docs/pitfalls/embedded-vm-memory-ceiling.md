# Embedded VM memory ceilings

This entry covers the Luau mechanisms present in the current tree. Earlier
Runtime v1 crop/report incidents and their deleted commands are not current
guidance and are left to Git history.

## The quota includes unreclaimed garbage

`modules/script/source/script/ffi/allocator.cpp` charges every live allocator
block to `MemoryQuota` and refuses a growth that would exceed the configured
limit. Luau's `VM/src/lmem.cpp` raises `LUA_ERRMEM` immediately when that
allocator returns null; it does not perform an emergency full collection and
retry.

The observed number is therefore allocated bytes still known to the VM, not a
minimum live-set measurement. Incremental collection may leave unreachable
objects charged until a later collection step. Where a host owns a natural
unit-of-script boundary, `collectGarbage` is the supported place to request a
full collection; re-entering the collector from the allocator callback is not.

## Protection is per `lua_State`

An allocation can fail before plugin bytecode starts: opening libraries,
creating an environment, loading bytecode, building an argument table, or
growing a coroutine stack all allocate. Protecting only `lua_resume` does not
protect those host-side operations.

Protection is also state-specific. A protected call installed on the main state
must not be expected to catch an exception raised by a child coroutine state.
The pure-data boundary therefore builds values under the protected main state,
reserves each child stack under a protected call on that child, transfers the
already-built values, and then uses `lua_resume` for plugin execution.

`tests/script/test-pure-data-boundary.cpp`, case “quota exhaustion while the host
pushes an input is a refusal”, constructs 300,000 empty arrays. They fit in the
boundary's JSON-size model but require one Luau table each, so the quota is
reached during host-to-Luau conversion. The required outcome is a returned
refusal containing `memory quota`, not process termination.

## Repeated strings can invalidate a memory test

Luau interns strings, including long strings. Repeating identical payload bytes
may therefore reuse one object and leave the heap peak nearly unchanged. A test
that is meant to measure repeated allocation must vary the bytes or use a shape
that necessarily creates distinct objects, then be falsified with the production
guard removed.

The same discipline applies to collector tests: enough interpreted work between
allocations can let incremental collection keep pace, so the test loop must have
the same allocation/work ratio as the path it claims to cover.

## Current regression evidence

- “an artifact is admitted only if it is JSON this VM can build” proves that a
  value exceeding the VM quota is refused during admission.
- “quota exhaustion while the host pushes an input is a refusal” proves that
  host-side value construction is protected.
- `MemoryQuota::peak` and `heapUsage` provide measurements; an error sentence
  without the ledger is not enough to distinguish quota pressure from another
  VM failure.

When adding a memory regression, remove or bypass the intended guard once and
confirm the named assertion fails for quota pressure. A test never observed red
is not evidence that its allocation shape reaches the ceiling.
