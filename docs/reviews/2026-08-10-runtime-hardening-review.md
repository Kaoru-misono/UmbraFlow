# Runtime hardening — independent review outcome (2026-08-10)

Two independent reviews were run over the state/persistence half and the
plugin/capability/annotation/Host half, as
[the handoff](../plans/2026-08-09-runtime-hardening-rewrite.md) requires before
the upstream rewrite can be called complete. Both returned FAIL on the first
pass. Every finding below is now closed or accepted with a stated reason, and
both halves were re-reviewed against the current tree.

## Closed

| ID | Severity | What it was | Where |
|---|---|---|---|
| A-F1 | high | `Rejected` reachable after an earlier `Continue` had committed a JournalEvent for the same Operation, against failure-and-recovery contract 15 | `ccb39df` |
| A-F2 | high | the reconciliation disposition was caller-supplied while the reconcile output already carried one | `5575ae4` |
| A-F3 | medium | `verifyClosure` read its enumeration error at the top of the loop body, so a failed final increment passed as a closed artifact | `ccb39df` |
| A-F4 | medium | `commitReconciliation` and `createOrLoadOperation` accepted a session that a restart had fenced out | `ccb39df` |
| A-F5 | medium | the idempotent-hit path returned another session's operation id and revision | `ccb39df` |
| A-F6 | low | staging writes followed links planted in directories the installer creates | `b2be20d` |
| A-F7 | low | a row's columns were read after its transaction committed | `5e8630e` |
| B-F1 | high | `PendingReceipt` stored a raw `TaskContext*` with no lifetime contract | `8dcba9c` |
| B-F2 | medium | the Tool Catalog owner named a `tool_catalog_hash` but accepted any validator for it | `5575ae4` |
| B-F3 | medium | the Journal payload schema hash was unverifiable; five registration accessors had no reader | `5575ae4` |
| B-F5 | low | `m_receipts` had no ceiling | `8dcba9c` |
| B-P1 | medium | agent-controlled documents had no per-row or total ceiling | `8dcba9c` |
| B-P2 | medium | Python's `AuthoringCapabilityRoot` did not satisfy the schema whose hash it stamps into every release | `ccb39df` |
| B-P3 | low | the trusted capability objects were ordinary mutable instances | `5e8630e` |

### P1-2 — path confinement

Closed in `8dcba9c` (reads) and `b2be20d` (staging writes).

`task/platform/confined-file` resolves a path once. The root handle is held for
the object's lifetime, every directory on the way to a file is held open while
that file is opened, and no component is traversed through a reparse point --
rejected by ATTRIBUTE rather than by tag, so a junction, an AppExecLink and a
cloud placeholder are refused alike, which `is_symlink` did not do. Handles are
opened without delete sharing, so a prefix cannot be renamed out from under a
later component. Writes create-new, so a link planted at the leaf fails instead
of being written through. POSIX gets the same shape from `openat`, `O_NOFOLLOW`
and `O_EXCL`.

One consequence is worth carrying forward: holding those handles is what stops
the prefix moving, and it equally stops a `rename`. The confined root over a
staging tree is therefore scoped to the writes and released before the tree is
published.

## Accepted, with reasons

**A-F8 — a failed installed-generation CAS leaves the published artifact
directory behind.** Deleting it would be wrong, not merely unnecessary: the
directory is content-addressed and re-verified on every open, and one of the
ways the CAS fails is a concurrent publisher having already put the identical
bytes there. Removing it on our own failure would break their installation to
tidy ours. Stated at the call site.

**B-F4 — the three authority-bearing values are copyable and carry no
consumption marker.** A holder can stash and replay one. Replay is bounded a
layer down rather than in the value: a tool invocation by the command
fingerprint and the idempotency key, a Journal entry by `event_id` uniqueness
and the ProjectState revision CAS, a reconcile outcome by the Operation
revision CAS. Making them move-only would ripple through every call site for a
property those CAS checks already provide.

## Tests that could not fail, and what they do now

Each replacement was falsified by removing the protection it names and
observing the case turn red.

| Was | Now |
|---|---|
| the only reparse case asserted a bitmask on a synthetic object, then exercised a symlink that `S_ISLNK` rejects on its own | creates a junction: no privilege needed, `lstat` reports a plain directory, and the reparse bit is the sole defence |
| two `except PermissionError` branches asserted nothing | each asserts that the refusal actually held |
| the duplicate-header case sent no duplicate header, because `http.client` takes a dict | sends the request literally over a socket and repeats each guarded header |
| no check-open-delete test existed | a capability file is swapped under its open descriptor |
| `pendingReceipt` fabricated a Receipt, so a public constructor would not have been noticed | delivery is now attempted with the wrong context, which only the cycle check refuses |
| the plugin surface case ORed five globals no code path registers | names only globals the whitelist actually excludes |
| only `ValidatedJournalEntryData` had an aggregate/constructible guard | every authority-bearing value has one |

There is no C++ symlink, junction or reparse coverage before this change at
all; `tests/task/test-confined-file.cpp` is the first.

### Known limit of one positive control

The capability-swap case defends through two different mechanisms and which one
fires is platform-dependent. On Windows the open handle refuses the replacement
outright, so the identity re-check in `_descriptor_document` never runs and the
case would pass with that comparison deleted. Its positive control is therefore
POSIX-only, where CI's `linux-analysis` job runs it. The case names both
mechanisms rather than passing on whichever holds.
