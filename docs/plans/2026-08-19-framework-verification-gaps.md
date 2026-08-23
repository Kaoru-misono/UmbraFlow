# Framework verification gaps

Date: 2026-08-19
Scope: `umbraflow-cpp` only

This plan owns framework obligations exposed while removing cross-repository
status transcription. It carries work and acceptance conditions, not consumer
status, foreign commit hashes or dated rulings. The rulings that created these
obligations are frozen under [`docs/decisions/`](../decisions/README.md).

## G-01 - Verify every project-manifest dependency

> **Reduced in scope, 2026-08-23**, by
> [the framework stops interpreting a Project's state](../decisions/2026-08-23-the-framework-stops-interpreting-project-state.md).
> A deployment names no declarative document by path any more: its Tools and its
> identity schemas are inline, so the only files it can name are its Luau
> modules and its resources, which `project build` already opens and stages. The
> declared-file record this gap once asked for was deleted with the file set it
> enumerated.

What remains of this gap is the semantic half: `project check` compiles no
inline schema, performs no Tool-name-to-entry join and derives no registration
root, so a declaration whose joins do not hold can still pass the kit and be
refused at `umbra-flow open`. Completion requires one canonical loader-backed
verification path. The implementation must not add a second parser to `project`;
either the loader boundary is made available without creating a dependency
cycle, or the ownership boundary is changed explicitly.

## G-02 - Make the Operator public-surface scan recurring

The ruling in
[`2026-08-17-public-surface-scan-and-harm-tiers.md`](../decisions/2026-08-17-public-surface-scan-and-harm-tiers.md)
requires repeatable detection of public Operator mutators that have only test
callers. Completion requires a check that reports candidates for human tiering;
it must not infer a defect from caller absence and must preserve explicit
exclusions such as reader methods intended for tests.

Prefer extending an existing repository check or contract test over adding a
standalone gate script. Prove the check can fail by introducing one temporary
public mutator with no production caller, then remove the mutation.

## G-04 - Close the remaining terminology-layer inconsistencies

The framework has two terminology debts identified by
[`2026-08-17-effect-envelope-and-expected-effect-are-two-layers.md`](../decisions/2026-08-17-effect-envelope-and-expected-effect-are-two-layers.md):

- reconcile the local `EffectEnvelope` helper in `effective-plan.cpp` with the
  published Operator schema without merging it with project `ExpectedEffect`;
- choose and propagate one offline/online Agent vocabulary in the framework.

Completion requires one canonical spelling per layer and a blast-radius sweep
through code, current documents and skills. Archived documents remain frozen.
