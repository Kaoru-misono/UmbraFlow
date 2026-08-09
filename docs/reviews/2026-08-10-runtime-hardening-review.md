# Runtime hardening — independent review outcome (2026-08-10)

Two independent reviews were run over the state/persistence half and the
plugin/capability/annotation/Host half, as
[the handoff](../plans/2026-08-09-runtime-hardening-rewrite.md) requires before
the upstream rewrite can be called complete.

**Both returned FAIL.** Some findings are closed; the rest are recorded below
and the delivery bar is therefore **not met**. Do not report the rewrite as
finished while this file has open entries.

## Closed in `ccb39df`

| ID | Severity | What it was |
|---|---|---|
| A-F1 | high | `Rejected` reachable after an earlier `Continue` had committed a JournalEvent for the same Operation, against failure-and-recovery contract 15 |
| A-F3 | medium | `verifyClosure` read its enumeration error at the top of the loop body, so a failed final increment was accepted as a closed artifact |
| A-F4 | medium | `commitReconciliation` and `createOrLoadOperation` did not require a current-epoch active session, so a fenced-out process could still write the Journal |
| A-F5 | medium | the idempotent-hit path returned another session's operation id and revision |
| B-F2 | medium | `ProjectToolCatalogSchemaOwner` recorded a `tool_catalog_hash` but accepted any validator for it |
| B-P2 | medium | Python's `AuthoringCapabilityRoot` did not satisfy the schema whose hash it stamps into every release manifest |
| B-k  | — | the only reparse test asserted a bitmask on a synthetic object; replaced with a junction case that goes red when the check is removed |

## Open

### A-F2 — high — the reconciliation disposition is still caller-supplied

`ReconciliationCommit::disposition` is an enum the caller sets, while
`commit.proposal` — the plugin's own `reconcile()` output, which already
carries a `disposition` member — is stored verbatim and never parsed
(`modules/operator/source/operator/ledger.cpp`, the proposal is read only for
registration, function and direction). A caller holding a proposal that
concluded "rejected" can commit it as `Confirmed` with a JournalEvent.

This is the same shape as the two P0 holes the rewrite closed, one field over:
`v1.7 §"disposition: Continue | Confirmed | Rejected | Ambiguous | Diverged"`
makes the conclusion the reconciler's, not the requester's. It also defeats the
A-F1 guard, because relabelling to `Confirmed` avoids the check entirely.

Fix: mint the disposition through an authority bound to the registration's
`reconcile_payload_schema_manifest_hash`, in the shape
`ProjectJournalSchemaOwner` and `ProjectToolCatalogSchemaOwner` already use, and
delete the caller field.

### B-F1 — high, latent — the Receipt stores a raw `TaskContext*`

`TaskHost::PendingReceipt::p_context` is a stored borrow with no backing-owner
contract; `m_receipts` is pruned only by a successful `deliver()`, so an
undelivered Receipt outlives the context it points at. Marked
`TODO(cpp-debt)` at the declaration. Latent only because `deliver()` has no
production caller while Phase 1 keeps production input closed — it must be
fixed before that surface opens.

### B-F3 — medium — the Journal payload schema hash is unverifiable

The recorded `payload_schema_hash` is whatever the deployment validator
returned and is never cross-checked. The registration field that should pin it,
`journalEventSchemaManifestHash`, has no accessor on
`VerifiedProjectRegistration` and no production reader — as is also true of
`manifestSchemaHash`, `projectObservationSchemaHash`,
`projectToolPreconditionSchemaHash` and `reconcilePayloadSchemaManifestHash`.
Same fix shape as B-F2.

### B-P1 — medium — no quota on agent-controlled documents

`candidate_revisions.document` and `agent_checkpoints.document` carry only
`CHECK(length(document) > 1)`: no byte cap and no row cap, while blobs have
both. An untrusted agent can grow the workspace database without bound, and
both tables are immutable or no-delete.

### A-F6, A-F8, B-F4, B-F5, B-P3 — low

Staging writes follow links (mitigated by a 32-byte CSPRNG leaf name and an
untrusted-root assumption stated nowhere); a failed generation CAS orphans a
published artifact directory; the three unforgeable tokens are copyable with no
consumption marker; `m_receipts` has no expiry sweep or cap; Python capability
objects are mutable in-process.

## P1-2 — RuntimeArtifact path confinement: answered, not fixed

Frozen bytes plus exact-hash verification **limit** the check/open race but do
not remove it, and the code does not meet v1.7's confinement-open requirement.
Every access is a `symlink_status`/`canonical` check on a path string followed
by a separate `ifstream`/`ofstream` open of the same string; no handle is
carried across the two.

What the hash chain does buy is complete: content substitution is defeated at
every level, and the Host never trusts bytes it did not authorise. What remains
is confinement. Two concrete residuals:

- an attacker with write access to `handoffRoot` can swap `release.manifest.json`
  between the check and the open for a symlink to a network path, hanging the
  installing thread with no timeout; aimed at a local file it becomes a
  confused-deputy hash oracle;
- `is_symlink` sees only name-surrogate reparse tags, so AppExecLink, cloud
  placeholder and container-isolation tags are followed transparently. The
  repository has no symlink, junction or reparse test on the C++ side at all.

The mechanism is already understood elsewhere in the same file: the SQLite
handle is opened with `SQLITE_OPEN_NOFOLLOW`. The fix is a handle-based walk
behind a `platform/`+`ffi/` boundary — open the artifact root once, resolve
each manifest-declared component relative to the parent handle, reject any
handle carrying `FILE_ATTRIBUTE_REPARSE_POINT` regardless of tag, confirm each
`FILE_ID_INFO` chains to the root, and do the reads and staging writes through
those handles without ever re-opening by path. A second `canonical()` would not
close it.

## Tests that pass without testing anything

Beyond the reparse case already replaced:

- two `except PermissionError:` branches assert nothing, so on a machine where
  the overwrite is denied the case is green with zero assertions;
- the duplicate-header case never sends a duplicate header, so the guards it
  aims at are untested (they do work — verified out of band with raw sockets);
- no check-open-delete TOCTOU test exists anywhere, so the identity re-checks
  in `safe_paths` have no positive control;
- `pendingReceipt` fabricates a `TaskHost::Receipt` from Host internals rather
  than capturing the script's, so nothing falsifies "a script cannot construct
  a Receipt";
- the plugin surface case ORs five names no code path ever registers, so those
  disjuncts hold with or without the whitelist; `coroutine`, `setmetatable`,
  `newproxy`, `collectgarbage` and `string.dump` are untested exclusions;
- `ValidatedToolInvocation` and `ValidatedDocument` have no
  `is_aggregate`/`is_constructible` guard, unlike `ValidatedJournalEntryData`.
