# Cross-repository audit: framework versus committed v1.18 bundle

Status: current report; supersedes the v1.9 audit dated 2026-08-11
Date: 2026-08-13

> Amended 2026-08-16: **the bundle pin this report verified no longer exists.**
> `scripts/check_spec_bundle.py` and the root are removed; the bundle is now
> identified by a semantic contract version, and byte pinning is kept only where
> a validator consumes the bytes. The measurements below are left as written —
> they are the record of 2026-08-13, and the reason the pin was removed is that
> those measurements held, not that they were wrong. The findings table's owners
> are unchanged except where noted inline.
Framework branch: `design/annotation-system-v2`
Consumer checkout: `E:/umbraflow-projects/uf-chaos/`
Execution authority: consumer `docs/architecture/parallel-implementation-plan.md`

Both repositories were read-only during this audit except for this framework
documentation report. The consumer worktree has one unrelated deleted compiled
content artifact; no bundle member is dirty.

## Verified bundle

`python scripts/check_spec_bundle.py` opened the live consumer checkout and
reported:

```text
Spec bundle check OK (6 files, 243474 bytes hashed).
SPEC BUNDLE: VERIFIED (v1.18 root ac8c3fa652fb1601645d0c0bc04359bc75c9d08dc2883aa31ddeb94912f38ec4, E:\umbraflow-projects\uf-chaos\docs\architecture)
```

The manifest is committed by consumer commit `edf9920c5726ae90aaede20f93066a1bea1ae5f3`
and contains five pinned documents. Its own 1,029 bytes hash to the stated
bundle root. Each declared document byte size and SHA-256 was independently
recomputed and matched. The implementation plan is deliberately outside the
bundle.

## Does this still require a separate v2.0 bundle?

No. The v1.9 report's demand for the label “v2.0” was a demand that conflicting
members and upstream-only resolutions move under one shared revision. The
consumer has since exercised exactly that mechanism through v1.18: the five
documents and bundle root move together, old interface-lock directories are
replaced rather than accepted in parallel, and the current bundle is committed
and checked from both repositories. A second version flag or parallel bundle
would recreate the ambiguity the rule exists to remove.

This does **not** declare the v1.18 prose internally perfect. Remaining
disagreements must be corrected by one later co-versioned bundle revision under
the existing change protocol. They do not require a second “v2.0” line.

## v1.9 findings re-audited

“Closed” means current bytes/code removed the disagreement. “Owner” names the
only consumer-plan work package allowed to change the remaining contract; this
report is not another execution ledger.

| Old finding | Current judgment | Unique owner if anything remains |
|---|---|---|
| `F-1` uncommitted, unpinned arbiter | **Closed.** v1.18 is committed; the full gate opened and hashed it. | none |
| `F-2` wrong checkout identity | **Closed.** The live path is `E:/umbraflow-projects/uf-chaos/`; current upstream authority and gate use it. | none |
| `F-3` reduced ToolDescriptor and invented global bounds | **Closed in implementation.** Full descriptor/bounds and policy-owned offers landed. | `U8` owns any follow-up contract wording |
| `F-4` takeover fence absent from production | Still a production-composition gap, not an `A-07` schema gap. | `U10a` |
| `F-5` consumer Luau targets retired host APIs | Old files are historical; the consumer's current build/runtime convergence is not an upstream compatibility obligation. | `CH-01` |
| `F-6` `page-model.toml` is a different document | **Closed by breaking replacement.** Runtime v2 is the sole model shape; no v1 reader remains. | none |
| `F-7` three non-JCS canonicalisers | **Closed for the shared interface path.** Common JCS/schema work landed. | `U1` owns the published shared schema boundary |
| `F-8` consumer never runs the project half of `A-04` | The consumer-facing behavioural gate remains a consumer integration obligation. | `CH-05` |
| `F-9` `C-11`/`A-04` drop the project label | The framework table names one implementation owner; the consumer half remains under its real plugin package rather than a second upstream owner. | `CH-05` |
| `F-10` obligation counts conflict | Brittle copied totals are retired; `D-09` still uses `PHASED` where an owner classification is expected. | `CH-03` |
| `F-11` policy not evaluated | **Closed.** U8 policy evaluation and offer filtering landed. | none |
| `F-12` requirements quoted with changed meanings | Capability/offer and snapshot-event cases landed. The remaining wrong-attribution vocabulary is separate. | `U11c` |
| `F-13` target ID had two spellings | **Closed.** Current code and DDL use `controlled_target_id`. | none |
| `F-14` architecture carries conflicting bundle versions | **Closed.** Current authority reads v1.18/root `ac8c3fa…f38ec4`; older values are dated history only. | none |
| `F-15` live docs name deleted functions/fingerprints | **Closed by D-001** in current authorities. | none |

## v1.9 internal-bundle findings re-audited

The shared versioning mechanism resolves the procedural defect, but several old
textual contradictions remain in v1.18. Each has exactly one package owner
below; the consumer authority needs the precise acceptance additions recorded
as cross-lane requests in the L4 report.

| Old finding | Current judgment | Unique owner |
|---|---|---|
| `B-1` artifact roots have two shapes | Still present: main design uses `project_artifact_roots[]` at line 518 and `project_artifact_roots_manifest_hash` at line 1174; project-layer design uses the array at line 87. | `U3` |
| `B-2` post-dispatch approval has no legal exit | **Closed** by the recoverable transition resolution and failure/recovery authority. | none |
| `B-3` session hash appears inside its own tuple | Current prose still appends `session_manifest_hash` to the tuple it says that hash covers. | `U3` |
| `B-4` exact session tuple disagrees about fields | Still part of the same manifest-shape contradiction. | `U3` |
| `B-5` registration `manifest_schema_hash` absent from bundle prose | **Closed by deletion:** the field is gone. `schema/umbraflow-project-registration-v1.schema.json` now requires `project_registration_format`, a generation the verifier refuses by naming both numbers, so there is no schema digest for the bundle prose to omit. | none |
| `B-6` `PHASED` occupies an ownership column | Still present at requirements-traceability line 70. | `CH-03` |
| `B-7` ProjectSnapshot closed list grows later | Runtime implementation now has one canonical snapshot, but bundle prose must be aligned with the landed availability/event fields. | `U9` |
| `B-8` Agent budgets/no-progress disagree | **Closed in implementation:** Agent/read is refused, the unused millisecond field is deleted, and one step counter remains. | `U9` owns bundle wording |
| `B-9` `VERIFIED_PRIMITIVE` is defined but unused | Still present as a dead classification at requirements-traceability line 18. | `CH-03` |
| `B-10` session-hash scope is glossed two ways | Still part of the manifest-shape contradiction. | `U3` |
| `B-11` Journal example omits required event families | The consumer Journal/content schema package owns the authoritative vocabulary and examples. | `CH-03` |
| `B-12` idempotency namespace has adjacent spellings | Still present: `idempotency_namespace` is followed by `authenticated_controller_namespace` in the same rule. | `U2b` |
| `B-13` JCS bytes are not stated once for both roots | Still part of the manifest canonical-byte contract. | `U3` |

## Current divergences not usefully described by v1.9

| Divergence | Evidence measured 2026-08-13 | Unique owner |
|---|---|---|
| Operator schema says `ui_observation`; deployment/code and bundle say `ui_snapshot` | `schema/umbraflow-operator-v1.schema.json:399,411` versus `modules/deployment/source/deployment/project-deployment.cpp:216,237` and bundle main design line 415. | `U11c` |
| Framework has a local `EffectEnvelope` helper while the published record is `ExpectedEffect` | `modules/operator/source/operator/effective-plan.cpp:144-192`; schema definition at `schema/umbraflow-operator-v1.schema.json:457`. | `U11c` |
| DDL carries `controller_kind`, published Operator schema does not | `ledger.cpp:1524-1525`; no corresponding schema hit. | `U2f` |
| Runtime-model prose contract itself still teaches pre-v2 records | The excluded file is `docs/plans/2026-08-09-runtime-model-contract.md`. | `D-002` |
| Full bundle check does not compare the hardening authority's printed pin with its Python pin | Mutating the authority root to a different digest still returned exit 0 and `SPEC BUNDLE: VERIFIED`; the checker reads that document only to locate the checkout. | ~~`U12d`~~ — **closed 2026-08-16 by deletion**: both the checker and the transcribed pin are gone, so there is nothing left to disagree. This finding was one of the three measurements that decided the removal. |

The audit therefore supersedes the v1.9 conclusion: one co-versioned bundle is
the correct mechanism, no compatibility bundle is justified, and every current
divergence has one named owner in the single consumer execution authority.
