# Annotation-v2 behavior fixtures

These fixtures are deliberately not `page-model.toml` files. They are small,
deterministic inputs for the resolver, receipt, offline-check, and replay tests
that will be implemented after the annotation-v2 contract is frozen.

The fixture format describes observable behavior rather than a runtime schema:

- `frames[].measurements[]` are synthetic observations produced from one frame;
- `candidates[]` describes the competing surface hypotheses used by the test;
- `request` describes the operation under test;
- `expected` is the contract the implementation must satisfy.

No fixture needs a screenshot, OCR model, template file, clock, or random seed.
The same fixture must produce the same resolution, diagnostics, receipt result,
and action result on every run.

## Measurement vocabulary

Every measurement has a stable `probe` name and one of three results:

- `present`: the probe was observed;
- `absent`: the probe was observed and is explicitly not present;
- `unknown`: the probe could not be evaluated with sufficient evidence.

`unknown` must never be silently converted to `absent`.

The `confidence` values are fixed test inputs, not implementation output. A
future harness may replace a measurement with a real template/OCR adapter, but
the expected behavior must remain the same.

## Candidate vocabulary

Candidates are test hypotheses, not deployment records. They name the surface
layer (`scene`, `overlay`, or `interrupt`), identity probes, and the targets
used by the requested action. The final runtime schema may encode these facts
differently; the behavior and expected result are the stable part of the test.

## Fixture inventory

| Fixture | Contract exercised |
| --- | --- |
| `01-ordinary-scene.json` | A unique scene resolves and authorizes its declared action. |
| `02-overlay-above-scene.json` | An overlay resolves above the compatible scene. |
| `03-interrupt.json` | An interrupt resolves above different base scenes. |
| `04-indistinguishable-surfaces.json` | Two equally supported surfaces produce `ambiguous`. |
| `05-ocr-unknown.json` | Unknown OCR evidence produces `unknown`, not `absent`. |
| `06-shared-leave-equip-target.json` | One target is usable from two surfaces without ownership state. |
| `07-strip-collection.json` | A collection yields stable item count and ordered item regions. |
| `08-page-specific-placement.json` | The active surface selects its own target placement. |
| `09-stale-receipt.json` | A receipt from an earlier frame cannot authorize a later action. |
| `10-unknown-dark-modal.json` | An unmodeled dark modal remains unknown and non-actionable. |
| `11-declared-observed-transition.json` | Declared policy and observed history remain separate. |
