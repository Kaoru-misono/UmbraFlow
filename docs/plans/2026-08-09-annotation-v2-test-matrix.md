# Annotation-v2 test matrix

Status: behavior contract for P7

Date: 2026-08-09

This matrix defines the smallest independent inputs that the future
annotation-v2 model, resolver, receipt, offline-check, and replay tests must
consume. It intentionally does not define or copy the deployment TOML schema.
The fixture format is described in
`tests/fixtures/annotation-v2/README.md`.

## Test harness boundary

The fixtures are synthetic observation records. They do not require:

- uf-chaos;
- screenshots or committed image assets;
- an OCR model or template matcher;
- network access;
- wall-clock waiting;
- random data.

Each case must be runnable by stable `case_id`, and the manifest must run them
in a deterministic order. A future C++/Luau test target may replace a
synthetic measurement with a real adapter, but it must preserve the expected
behavior below.

The fixture result vocabulary is:

```text
evidence: present | absent | unknown
resolution: resolved | ambiguous | unknown
receipt: minted | not_minted
action: authorized | rejected
```

`unknown` is not negative evidence. An unreviewed or unevaluable measurement
must not satisfy a required identity predicate and must not be rewritten as
`absent`.

## Matrix

| ID | Fixture | Input focus | Required result | Primary owner |
| --- | --- | --- | --- | --- |
| T01 | `scene.ordinary` | One scene identity is present; its target is present and bound to `click`. | `resolved`, stack `camp`, receipt minted, action authorized. | Runtime model + resolver |
| T02 | `surface.overlay_above_scene` | Scene and compatible overlay are both present. | `resolved`, ordered stack `[battle, enemy_inspect]`; top overlay action authorized. | Resolver |
| T03 | `surface.interrupt_above_any_scene` | The same interrupt is tested once over battle and once over camp. | Each frame resolves with its base scene below `network_retry`; retry action is authorized. | Resolver + receipt |
| T04 | `resolution.indistinguishable_surfaces` | `shop` and `equip` have equal identity evidence and shared `leave_button`. | `ambiguous`, both candidates reported, no receipt, action rejected. | Resolver |
| T05 | `resolution.ocr_unknown` | Required title OCR is `unknown` at low confidence. | `unknown`, no receipt, click rejected; result must not become `absent`. | Observation + resolver |
| T06 | `target.shared_across_surfaces` | `leave_button` and `equip_button` are bound by both shop and loadout. | Each unique surface authorizes its requested shared target; no ownership concept is needed. | Model + resolver |
| T07 | `target.collection_items` | One repeated target reports three ordered item rectangles. | `items` read returns count 3, indices `[0,1,2]`, and the exact ordered rectangles. | Reader/geometry |
| T08 | `binding.surface_specific_placement` | The same `confirm_button` has different rectangles on two surfaces. | Each frame authorizes using the active surface rectangle; no global rectangle is used. | Binding + action |
| T09 | `receipt.stale_frame` | Receipt is minted on `before`, then used on `after` in the same ticket. | Current resolution may succeed, but old receipt action is rejected as stale. | Evidence/receipt |
| T10 | `resolution.unknown_dark_modal` | Known surfaces are absent; visible dark modal and dismiss target are unknown. | `unknown`, no catch-all surface, no receipt, dismiss rejected, diagnostic emitted. | Resolver safety |
| T11 | `transition.declared_vs_observed` | Declared camp→training-confirm transition; observed camp→shop transition. | Declared policy remains present; observation is unexpected; model is not rewritten. | Navigation/replay |

## Detailed contracts

### T01: ordinary scene

The resolver must be able to produce a single-surface runtime state from one
positive identity probe. A binding can authorize an action only after that
state is resolved from the same frame.

### T02: overlay above scene

Both the lower scene and overlay may match the same frame. The result is not a
choice between them: it is an ordered visible stack. The overlay is the active
surface for its own action bindings.

### T03: interrupt

Interrupt selection is independent of the base scene. The test runs the same
interrupt over two different scenes to prevent an implementation from treating
interrupts as an overlay with one hard-coded parent.

### T04: indistinguishable surfaces

Equal evidence is a safety failure, not a tie to resolve by declaration order.
The diagnostic must retain both candidate surface IDs. No actionable receipt
may be minted from this result.

### T05: OCR unknown

The resolver must carry the unknown state and its reason/confidence through to
the final result. A required identity predicate is not satisfied by unknown,
and unknown must not be interpreted as explicit absence.

### T06: shared targets

The same target identifier can occur in multiple surface bindings. Runtime
resolution chooses the active binding; it must not require target ownership,
page ownership, or a mutable global owner field.

### T07: strip/collection

Collection reading is a single deterministic result with stable order and
per-item geometry. It must not collapse the collection into one scalar target
or silently reorder items.

### T08: page-specific placement

Placement is binding-specific. The active surface determines the hit geometry
for the same target identifier. A target-global rectangle is an explicit
forbidden behavior in this case.

### T09: stale receipt

Receipt authorization is bound to the observation frame, not merely to a
ticket or a target name. A later frame may resolve successfully, but it cannot
reuse an earlier receipt.

### T10: unknown dark modal

An unmodeled visible modal is an `UnknownResolution`. There is no authored
catch-all surface and no fallback action target. The diagnostic should make the
unmatched visible content discoverable to offline tooling.

### T11: declared versus observed transition

The declared transition is runtime policy. The observed transition is offline
evidence. A mismatch produces a replay/check finding and never silently edits
the declared model.

## Required implementation contracts

The following are the minimum contracts that P1/P3/P4 and later integration
work must implement against these fixtures.

### Model and compiler

- Represent scene, overlay, and interrupt layer kinds explicitly.
- Permit a target to appear in multiple surface bindings.
- Permit binding-specific placement and action grants.
- Represent collection geometry and ordered item reading.
- Reject an authored catch-all runtime surface.
- Keep declared transitions separate from observed transition records.
- Make model compilation deterministic for identical input.

### Observation and resolver

- Preserve `present`, `absent`, and `unknown` as distinct results.
- Evaluate all relevant candidates from one observation cycle.
- Build an ordered surface stack for compatible scene plus overlay matches.
- Evaluate interrupts independently of the base scene.
- Return `ambiguous` when equally supported surfaces remain.
- Return `unknown` when required evidence is unknown or no known surface fits.
- Never use first-match ordering to break an unresolved tie.

### Receipt and action authorization

- Mint receipts only for `resolved` states.
- Bind a receipt to the frame identity and ticket that produced it.
- Reject a receipt used against a later frame, even in the same ticket.
- Authorize only actions declared by the active surface binding.
- Resolve shared targets through the active binding, not ownership metadata.
- Reject all actions from `ambiguous` and `unknown` results.

### Offline graph and replay

- Store declared and observed transitions as different record types.
- Report an observed destination that differs from policy.
- Do not rewrite the runtime model from one observation.
- Treat absent assertion data as unreviewed, not as negative evidence.

## Expected future test shape

The future test target should expose tests equivalent to:

```text
load_manifest()
for case in manifest.cases:
    fixture = load_fixture(case)
    result = run_behavior_case(fixture)
    assert_result(case, result)
```

The harness may be C++ or Luau, but it should test the public result types and
not reach into implementation-specific tables. The fixture files are the
stable cross-agent contract; runtime and resolver agents must not modify them
to make an implementation pass.

