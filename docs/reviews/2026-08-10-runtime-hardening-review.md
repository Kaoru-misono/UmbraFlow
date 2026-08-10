# Runtime hardening — independent review outcome (2026-08-10)

Two independent reviews were run over the state/persistence half and the
plugin/capability/annotation/Host half, as
[the handoff](../plans/2026-08-09-runtime-hardening-rewrite.md) requires before
the upstream rewrite can be called complete. Both returned FAIL, were fixed,
re-reviewed, and returned FAIL again with further findings — two of them
regressions introduced by the first round of fixes. Everything below is now
closed or accepted with a stated reason.

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
| B-P2 | medium | Python's `AuthoringCapabilityRoot` did not satisfy, and never validated against, the schema whose hash it stamps into every release | `ccb39df`, `564eda0` |
| B-P3 | low | the trusted capability objects were ordinary mutable instances | `5e8630e`, `564eda0` |

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

### A-F8 — the artifact directory a failed CAS leaves behind

Closed 2026-08-10 by work item W8 of
[the next block](../plans/2026-08-10-next-block.md), which was ordered on the
strength of this finding's acceptance.

The acceptance below was right and is still the reason nothing is deleted on
failure: the directory is content-addressed, a concurrent publisher may have
put the identical bytes there, and removing it on our own failure would break
their installation to tidy ours. That reasoning ruled out deletion at the call
site. It did not rule out reclaiming the directory later, once the whole
reference set can be read at one time — which is what the finding actually
asked for and what was missing.

The reference set is now a set of foreign keys rather than a counter:
`runtime_installations` rows, a new `runtime_publications` table for an
installation in flight, and `runtime_state.active_runtime_artifact_root_hash`.
`reclaimUnreferencedRuntimeArtifacts` removes exactly the artifact directories
no row in those three names, inside a `BEGIN IMMEDIATE` that a publisher's own
claim must either precede or follow. A failed CAS therefore still leaves the
directory alone; what it leaves is a row nothing names, and that row is
reclaimable.

The Operator ledger DDL fingerprint moved with the new tables, to
`5738e6f98534efbdfc3114413de70c032b64e2cbaa84d4c152ec6cbb512120a4`. An existing
operator database no longer opens. Nothing is released, so those databases are
recreated rather than migrated, and there is exactly one fingerprint in the
tree.

> **Read as of 2026-08-11: one of those three legs is gone, and A-F8 stays
> closed.** The paragraphs above describe the tree of 2026-08-10 and are left as
> written. Since then the third adversarial round's R3-F2 showed that no test
> could observe a non-empty `runtime_publications` and that the comment
> defending it was not supported by the code, and `848e390` deleted the table
> rather than leaving it under a debt marker. The reference set is now
> `runtime_installations` and `runtime_state.active_runtime_artifact_root_hash`,
> and the fingerprint is
> `sha256:12f64bfff305c30c716fbd5bdc9934a17140dfe4e127b5bce2ec7a10ecd309e4`.
> What closed A-F8 was reading the whole reference set at one time under
> `BEGIN IMMEDIATE`, and that is unchanged by the number of legs in it.

## Accepted, with reasons

**A-F8 — a failed installed-generation CAS leaves the published artifact
directory behind.** Deleting it would be wrong, not merely unnecessary: the
directory is content-addressed and re-verified on every open, and one of the
ways the CAS fails is a concurrent publisher having already put the identical
bytes there. Removing it on our own failure would break their installation to
tidy ours. Stated at the call site.

> Superseded 2026-08-10: this reasoning stands, but the finding no longer
> stands accepted — see A-F8 under Closed above.

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

There was no C++ symlink, junction or reparse coverage before this change at
all; `tests/task/test-confined-file.cpp` is the first.

### Known limit of one positive control

The capability-swap case defends through two different mechanisms and which one
fires is platform-dependent. On Windows the open handle refuses the replacement
outright, so the identity re-check in `_descriptor_document` never runs and the
case would pass with that comparison deleted. Its positive control is therefore
POSIX-only. The case names both mechanisms rather than passing on whichever
holds.

An earlier version of this file said that control runs in CI's
`linux-analysis` job. That was wrong, and the re-review caught it: the whole
Python suite ran in no gate at all — not `ctest`, not `ci-local`, not the
workflow — so every property it proves was unenforced between runs somebody
remembered to do by hand. It is registered as `test-annotate-backend` with the
`CI` label since `564eda0`, which is why the suite is 61 tests rather than 60.

## Second round

The re-reviews found seven and five further items. Closed in `564eda0`:

| ID | Severity | What it was |
|---|---|---|
| A-NEW-1 | high | `Rejected` still reachable with a dispatch whose outcome was NULL or `transport_unknown`; only `not_delivered` proves absence |
| A-NEW-2 | high | a reconcile outcome was bound to a registration but not to an Operation, so a conclusion could be moved between two that were both reconciling |
| A-NEW-3 | medium | `commitReconciliation` and `transitionOperation` accepted a controller displaced by a human takeover |
| A-NEW-4 | medium | a dead reader left by the first round would fail `-Werror` on Linux and macOS, which the Windows-only gate could not see |
| A-NEW-5 | low-medium | the Windows walk re-resolved from the root path string, so a rename above the root redirected the prefix |
| A-NEW-6 | low | the component split refused `..` but not a backslash |
| A-NEW-7 | low | the junction cases returned quietly when the link could not be made |
| B-NEW-1 | high | `ProjectSchemaOwner`, the largest authority, was still bound to nothing |
| B-NEW-2 | high | **regression**: the receipt ceiling wedged the runtime, because nothing prunes and no production deliverer exists |
| B-NEW-3 | medium | **false claim**: the Python suite ran in no gate |
| B-NEW-4 | low | a failed capability open closed the same descriptor twice |
| B-NEW-5 | low | the document quota summed characters against a byte ceiling |
| (e) | — | **regression**: the wrong-context delivery case could not fail, because the second context held no cycle |

Two of these were caused by the first round of fixes. A ceiling with no
eviction and a test whose refusal came from the wrong cause are both the shape
of mistake that only a second adversarial pass finds.
