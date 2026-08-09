# Architecture

This document is the active ownership contract for the annotation/runtime
rewrite. The current contract is defined by
[`2026-08-09-runtime-model-contract.md`](plans/2026-08-09-runtime-model-contract.md)
and the four schemas under [`schema/`](../schema/). Older plans describe
superseded designs; they are not implementation authority.

## Build and module shape

The repository uses April2's manifest-driven CMake module loader. Each direct
child of `modules/` with a `manifest.txt` becomes a CMake library target named
`${PROJECT_NAME}_<module>`. The root `manifest.txt` owns application name and
version; module manifests own reusable-library metadata.

The dependency direction remains acyclic:

```text
entry -> task -> engine -> {ocr, trace, vision}
                    \-> script -> {core, domain}
vision -> {core, domain, image}
ocr    -> {core, domain, vision}
trace  -> {core, domain, vision}
image  -> {core, domain}
controller (Windows) -> {core, domain}
```

`core` is the platform-free leaf. `controller` owns Windows target discovery,
capture sessions, and input delivery. `engine` owns the frame/action ports,
observation-cycle fencing, leases, and target-generation checks. `script` owns
the sandbox boundary. `task` packages the trusted Luau framework and the task
lifecycle. `trace` is the JSONL evidence stream shared by runtime, check, and
replay.

## Runtime versus offline ownership

The system has one hard deployment boundary:

```text
offline annotation workspace
  frames + screenshots + OCR observations + CandidateModel + review history
  + regressions + provenance
             |
             | Agent proposals -> human decisions -> validate -> compile
             v
page-model.toml + content-addressed locator assets
             |
             | C++ reads the envelope; trusted Luau reads and compiles the model
             v
Umbraflow runtime
  live frame -> evidence -> Resolution -> Receipt -> authorized Action
```

`page-model.toml` is the runtime envelope filename. It contains deployment
data only: schema version, geometry fingerprint, contexts, targets, locators,
readers, surfaces, bindings, declared transitions, and references to runtime
locator assets. It never carries annotation screenshots, raw OCR records,
CandidateModel data, Agent reasoning, review records, regression matrices, or
observed transitions. A live frame may exist temporarily during an observation
cycle; it is not the offline corpus.

The complete `RuntimeModel` is read from the envelope and compiled by the
trusted Luau framework. The C++ host must not implement a second semantic
parser. Before the VM exists, C++ reads only the pre-VM envelope facts:

- schema version;
- exact-file content hash;
- base resolution and DPI;
- target IDs and surface IDs.

C++ may reject malformed or oversized envelope files and validate literal
names used before VM startup. It does not interpret identity predicates,
binding placement, locator thresholds, surface selection, transitions, or
action policy. Those are Luau semantics.

## Runtime responsibilities

| Concept | Runtime responsibility | Does not own |
| --- | --- | --- |
| `Context` | Business/runtime environment used to constrain compatible surfaces and transitions. | Pixels or locator mechanics. |
| `Surface` | One visible layer: `scene`, `overlay`, or `interrupt`; declares context compatibility and overlay coverage. | The complete state by itself. |
| `Target` | Reusable semantic object and stable geometry/locator/reader references. | Surface identity or permission. |
| `Locator` | A way to find a target: template, text, geometry, relative, or collection. | Surface meaning. |
| `Reader` | Typed decoding of a target region: line, block, items, or presence. | Identity or action permission. |
| `Binding` | Connects one target to one surface; owns placement, identity evidence, exposed readers, and granted actions. | Global target ownership. |
| `Action` | Typed input contract with locator and same-cycle preconditions. | A generic escape hatch. |
| `Transition` | Declared policy from one exact runtime state to another. | Proof that the transition was observed. |
| `Resolution` | Result of one observation cycle: `Resolved`, `Ambiguous`, or `UnknownResolution`. | A persistent screenshot record. |
| `Receipt` | Framework-minted authorization tied to ticket, cycle, frame, model hash, binding, action, and proof. | User-constructed data with the same fields. |

`RuntimeState` is a context plus an ordered surface stack from bottom to top.
An overlay is valid only above a compatible covered surface. An interrupt may
appear above any compatible base state. There is no authored catch-all
surface; an unmodeled visible UI is an `UnknownResolution`.

Identity, reading, placement, and action are deliberately separate:

- identity predicates decide whether a binding supports a surface candidate;
- readers decode a target only when that binding exposes the reader;
- placement decides where the binding's target is actionable on that surface;
- actions grant only the named input and its same-cycle preconditions.

A shared target therefore has multiple bindings with independent placement and
permissions. No global owner flag is needed.

## Evidence and safety semantics

Every observation cycle captures once and reuses that frame for resolution,
reading, locating, and action authorization. Evidence is tri-state:

```text
Present(value, confidence, proof)
Absent(proof)
Unknown(reason, confidence?, proof?)
```

`Unknown` means not measured, below a reader floor, unreadable, stale,
incompatible, or failed internally. It is never converted to `Absent`.

Resolution rules are fail-closed:

1. evaluate all relevant candidates from the same cycle;
2. build a legal scene/overlay/interrupt stack;
3. return `Resolved` only when the stack is supported and unambiguous;
4. return `Ambiguous` when two candidates remain equally supported or a
   required distinction has no safe margin;
5. return `UnknownResolution` when no known stack fits or required evidence is
   unknown.

Candidate evaluation may use the deterministic layer order interrupt,
compatible overlay, then scene as a search strategy. That order never breaks
an unresolved tie. `Ambiguous` and `UnknownResolution` cannot mint a receipt.

An action requires a framework-minted receipt from the current ticket and
cycle, the active binding's explicit action, fresh locator proof, all positive
preconditions, the current model hash, and a non-ambiguous `Resolved` state.
Stale receipts, forged tables, missing proof, unknown evidence, and ambiguous
resolution are rejected.

## Offline annotation and Agent ownership

The offline workspace owns the evidence corpus and authoring process:

```text
Frame -> Observation -> Assertion -> CandidateModel -> Patch -> review
                                                        |
                                      accepted + validated + compiled
                                                        v
                                             RuntimeModel / page-model.toml
```

The Agent has six stages:

1. **Capture** imports traces and frames, deduplicates them, and records
   provenance.
2. **Perception** measures OCR, regions, templates, repeated geometry, and
   confidence; it never grants permission.
3. **Structure** clusters surfaces, contexts, overlays, shared targets, and
   distinguishing evidence.
4. **Contract** proposes runtime objects, bindings, readers, actions, and
   declared transitions.
5. **Verification** compiles a candidate, replays all evidence, and reports
   unknown, ambiguous, uncovered, invalid-action, and coverage findings.
6. **Repair** turns findings into semantic patches; it cannot silently promote
   an unresolved candidate.

`CandidateModel` is an offline, revisioned, reviewable object. A patch names
its semantic change, evidence, provenance, confidence, conflicts, risk, and
status. The backend writes no runtime file while a patch is merely proposed.

The decision queue must show supporting frames, competing candidates,
expected runtime effect, blast radius, open conflicts, and validation status.
Human approval is required for names, merges/splits, context or overlay
meaning, identity predicates that determine recognition, any action grant or
revocation, transition meaning, and every high/critical patch. Unknown-to-
known promotion, arbitrary ambiguity resolution, and shared-target permission
are never automatic.

There is no bulk approval path for high or critical patches. Low-risk
mechanical proposals may be batched only when they have no open conflicts, no
action or identity effect, and the queue records the exact set and revision.

## Promotion, compile, replay, and rollback

Promotion is an explicit, reviewable sequence:

1. Agent creates or updates a `CandidateModel` in the offline workspace.
2. Human decisions accept/reject individual patches at a known candidate
   revision; stale revisions are refused.
3. The canonical validator checks schema shape, references, layer legality,
   identity, reader ownership, placement, action preconditions, and
   transitions.
4. A dry-run compile renders a candidate runtime model and reports its content
   hash; dry-run is the default.
5. Evidence replay and regression checks must pass with no unresolved
   high/critical conflict before `write=true` is permitted.
6. The compiler writes a new immutable model artifact and records the
   candidate revision, model hash, reviewer decisions, and validation result.
7. Runtime deployment verifies the envelope hash and geometry fingerprint.

Replay is offline: it compares recorded observations and transitions with the
declared model and reports findings. It does not rewrite the model from what
was observed. A failed compile, unresolved ambiguity, or replay regression
blocks promotion.

Rollback selects a previously compiled, validated model artifact by content
hash, restores the matching locator assets, and records the rollback reason.
It does not mutate the annotation corpus or silently discard review history.
The next repair starts from a new CandidateModel revision.

## Source ownership

| Area | Owner |
| --- | --- |
| Capture target, live frames, leases, input, ticket fencing | C++ engine/controller |
| Raw vision/OCR primitives | C++ vision/OCR ports and adapters |
| Runtime model semantics, resolver, receipts, transitions | Trusted Luau framework |
| Envelope facts and model/geometry identity before VM startup | C++ host boundary |
| Frames, observations, assertions, candidates, patches, review audit | Offline annotation workspace |
| Schema generation and semantic validation | Canonical runtime/offline contracts plus Luau compiler protocol |
| Project names, domain policy, accepted runtime artifact | Project integration layer |

The annotation backend and UI consume the canonical schemas and compiler
protocol. They do not reimplement runtime semantics. Project scripts consume
the compiled model language; they do not implement global surface ordering,
receipt minting, or evidence ledgers.

## Repository boundaries

- `modules/engine/`: platform-independent capture/action ports and cycle
  fencing.
- `modules/controller/`: Windows capture and input composition.
- `modules/task/runtime/`: trusted Luau model, compiler, resolver, receipts,
  navigation, replay, and offline-check entry points.
- `tools/annotate/`: offline CandidateModel backend and decision queue UI.
- `schema/`: field-shape contracts; no tool may invent a competing enum set.
- `docs/plans/2026-08-09-*.md`: current runtime, Agent, work-breakdown, and
  fixture contracts.
- `docs/pitfalls/`: reusable failure modes; entries must describe the current
  Context/Surface/Target vocabulary.

The C++ module graph and core safety conventions remain governed by manifests,
checked values, RAII, and explicit unsafe/FFI boundaries. Those mechanics do
not change the runtime/offline ownership contract above.
